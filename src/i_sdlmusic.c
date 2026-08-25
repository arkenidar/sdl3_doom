//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	System interface for music.
//


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "SDL3/SDL.h"
#include "SDL3_mixer/SDL_mixer.h"

#include "config.h"
#include "doomtype.h"
#include "memio.h"
#include "mus2mid.h"

#include "deh_str.h"
#include "gusconf.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_swap.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "sha1.h"
#include "w_wad.h"
#include "z_zone.h"

#define MAXMIDLENGTH (96 * 1024)
#define MID_HEADER_MAGIC "MThd"
#define MUS_HEADER_MAGIC "MUS\x1a"

#define FLAC_HEADER "fLaC"
#define OGG_HEADER "OggS"

// Looping Vorbis metadata tag names. These have been defined by ZDoom
// for specifying the start and end positions for looping music tracks
// in .ogg and .flac files.
// More information is here: http://zdoom.org/wiki/Audio_loop
#define LOOP_START_TAG "LOOP_START"
#define LOOP_END_TAG   "LOOP_END"

// FLAC metadata headers that we care about.
#define FLAC_STREAMINFO      0
#define FLAC_VORBIS_COMMENT  4

// Ogg metadata headers that we care about.
#define OGG_ID_HEADER        1
#define OGG_COMMENT_HEADER   3

// Structure for music substitution.
// We store a mapping based on SHA1 checksum -> filename of substitute music
// file to play, so that substitution occurs based on content rather than
// lump name. This has some inherent advantages:
//  * Music for Plutonia (reused from Doom 1) works automatically.
//  * If a PWAD replaces music, the replacement music is used rather than
//    the substitute music for the IWAD.
//  * If a PWAD reuses music from an IWAD (even from a different game), we get
//    the high quality version of the music automatically (neat!)

typedef struct
{
    sha1_digest_t hash;
    char *filename;
} subst_music_t;

// Structure containing parsed metadata read from a digital music track:
typedef struct
{
    boolean valid;
    unsigned int samplerate_hz;
    int start_time, end_time;
} file_metadata_t;

static subst_music_t *subst_music = NULL;
static unsigned int subst_music_len = 0;

static const char *subst_config_filenames[] =
{
    "doom1-music.cfg",
    "doom2-music.cfg",
    "tnt-music.cfg",
    "heretic-music.cfg",
    "hexen-music.cfg",
    "strife-music.cfg",
};

static boolean music_initialized = false;

// If this is true, this module initialized SDL sound and has the 
// responsibility to shut it down

static boolean sdl_was_initialized = false;

static boolean musicpaused = false;
static int current_music_volume;

char *timidity_cfg_path = "";

static char *temp_timidity_cfg = NULL;

// If true, we are playing a substitute digital track rather than in-WAD
// MIDI/MUS track, and file_metadata contains loop metadata.
static boolean playing_substitute = false;
static file_metadata_t file_metadata;

// The mixer that music is played through, and the single track that all
// music is mixed on. SDL3_mixer has no dedicated "music" channel any more:
// music is just another MIX_Audio played on a MIX_Track.
static MIX_Mixer *music_mixer = NULL;
static MIX_Track *music_track = NULL;

// Currently playing music track.
static MIX_Audio *current_track_music = NULL;

// If true, the currently playing track is being played on loop.
static boolean current_track_loop;

// Given a time string (for LOOP_START/LOOP_END), parse it and return
// the time (in # samples since start of track) it represents.
static unsigned int ParseVorbisTime(unsigned int samplerate_hz, char *value)
{
    char *num_start, *p;
    unsigned int result = 0;
    char c;

    if (strchr(value, ':') == NULL)
    {
	return atoi(value);
    }

    result = 0;
    num_start = value;

    for (p = value; *p != '\0'; ++p)
    {
        if (*p == '.' || *p == ':')
        {
            c = *p; *p = '\0';
            result = result * 60 + atoi(num_start);
            num_start = p + 1;
            *p = c;
        }

        if (*p == '.')
        {
            return result * samplerate_hz
	         + (unsigned int) (atof(p) * samplerate_hz);
        }
    }

    return (result * 60 + atoi(num_start)) * samplerate_hz;
}

// Given a vorbis comment string (eg. "LOOP_START=12345"), set fields
// in the metadata structure as appropriate.
static void ParseVorbisComment(file_metadata_t *metadata, char *comment)
{
    char *eq, *key, *value;

    eq = strchr(comment, '=');

    if (eq == NULL)
    {
        return;
    }

    key = comment;
    *eq = '\0';
    value = eq + 1;

    if (!strcmp(key, LOOP_START_TAG))
    {
        metadata->start_time = ParseVorbisTime(metadata->samplerate_hz, value);
    }
    else if (!strcmp(key, LOOP_END_TAG))
    {
        metadata->end_time = ParseVorbisTime(metadata->samplerate_hz, value);
    }
}

// Parse a vorbis comments structure, reading from the given file.
static void ParseVorbisComments(file_metadata_t *metadata, FILE *fs)
{
    uint32_t buf;
    unsigned int num_comments, i, comment_len;
    char *comment;

    // We must have read the sample rate already from an earlier header.
    if (metadata->samplerate_hz == 0)
    {
	return;
    }

    // Skip the starting part we don't care about.
    if (fread(&buf, 4, 1, fs) < 1)
    {
        return;
    }
    if (fseek(fs, LONG(buf), SEEK_CUR) != 0)
    {
	return;
    }

    // Read count field for number of comments.
    if (fread(&buf, 4, 1, fs) < 1)
    {
        return;
    }
    num_comments = LONG(buf);

    // Read each individual comment.
    for (i = 0; i < num_comments; ++i)
    {
        // Read length of comment.
        if (fread(&buf, 4, 1, fs) < 1)
	{
            return;
	}

        comment_len = LONG(buf);

        // Read actual comment data into string buffer.
        comment = calloc(1, comment_len + 1);
        if (comment == NULL
         || fread(comment, 1, comment_len, fs) < comment_len)
        {
            free(comment);
            break;
        }

        // Parse comment string.
        ParseVorbisComment(metadata, comment);
        free(comment);
    }
}

static void ParseFlacStreaminfo(file_metadata_t *metadata, FILE *fs)
{
    byte buf[34];

    // Read block data.
    if (fread(buf, sizeof(buf), 1, fs) < 1)
    {
        return;
    }

    // We only care about sample rate and song length.
    metadata->samplerate_hz = (buf[10] << 12) | (buf[11] << 4)
                            | (buf[12] >> 4);
    // Song length is actually a 36 bit field, but 32 bits should be
    // enough for everybody.
    //metadata->song_length = (buf[14] << 24) | (buf[15] << 16)
    //                      | (buf[16] << 8) | buf[17];
}

static void ParseFlacFile(file_metadata_t *metadata, FILE *fs)
{
    byte header[4];
    unsigned int block_type;
    size_t block_len;
    boolean last_block;

    for (;;)
    {
        long pos = -1;

        // Read METADATA_BLOCK_HEADER:
        if (fread(header, 4, 1, fs) < 1)
        {
            return;
        }

        block_type = header[0] & ~0x80;
        last_block = (header[0] & 0x80) != 0;
        block_len = (header[1] << 16) | (header[2] << 8) | header[3];

        pos = ftell(fs);
        if (pos < 0)
        {
            return;
        }

        if (block_type == FLAC_STREAMINFO)
        {
            ParseFlacStreaminfo(metadata, fs);
        }
        else if (block_type == FLAC_VORBIS_COMMENT)
        {
            ParseVorbisComments(metadata, fs);
        }

        if (last_block)
        {
            break;
        }

        // Seek to start of next block.
        if (fseek(fs, pos + block_len, SEEK_SET) != 0)
        {
            return;
        }
    }
}

static void ParseOggIdHeader(file_metadata_t *metadata, FILE *fs)
{
    byte buf[21];

    if (fread(buf, sizeof(buf), 1, fs) < 1)
    {
        return;
    }

    metadata->samplerate_hz = (buf[8] << 24) | (buf[7] << 16)
                            | (buf[6] << 8) | buf[5];
}

static void ParseOggFile(file_metadata_t *metadata, FILE *fs)
{
    byte buf[7];
    unsigned int offset;

    // Scan through the start of the file looking for headers. They
    // begin '[byte]vorbis' where the byte value indicates header type.
    memset(buf, 0, sizeof(buf));

    for (offset = 0; offset < 100 * 1024; ++offset)
    {
	// buf[] is used as a sliding window. Each iteration, we
	// move the buffer one byte to the left and read an extra
	// byte onto the end.
        memmove(buf, buf + 1, sizeof(buf) - 1);

        if (fread(&buf[6], 1, 1, fs) < 1)
        {
            return;
        }

        if (!memcmp(buf + 1, "vorbis", 6))
        {
            switch (buf[0])
            {
                case OGG_ID_HEADER:
                    ParseOggIdHeader(metadata, fs);
                    break;
                case OGG_COMMENT_HEADER:
		    ParseVorbisComments(metadata, fs);
                    break;
                default:
                    break;
            }
        }
    }
}

static void ReadLoopPoints(char *filename, file_metadata_t *metadata)
{
    FILE *fs;
    char header[4];

    metadata->valid = false;
    metadata->samplerate_hz = 0;
    metadata->start_time = 0;
    metadata->end_time = -1;

    fs = fopen(filename, "r");

    if (fs == NULL)
    {
        return;
    }

    // Check for a recognized file format; use the first four bytes
    // of the file.

    if (fread(header, 4, 1, fs) < 1)
    {
        fclose(fs);
        return;
    }

    if (memcmp(header, FLAC_HEADER, 4) == 0)
    {
        ParseFlacFile(metadata, fs);
    }
    else if (memcmp(header, OGG_HEADER, 4) == 0)
    {
        ParseOggFile(metadata, fs);
    }

    fclose(fs);

    // Only valid if at the very least we read the sample rate.
    metadata->valid = metadata->samplerate_hz > 0;
}

// Given a MUS lump, look up a substitute MUS file to play instead
// (or NULL to just use normal MIDI playback).

static char *GetSubstituteMusicFile(void *data, size_t data_len)
{
    sha1_context_t context;
    sha1_digest_t hash;
    char *filename;
    int i;

    // Don't bother doing a hash if we're never going to find anything.
    if (subst_music_len == 0)
    {
        return NULL;
    }

    SHA1_Init(&context);
    SHA1_Update(&context, data, data_len);
    SHA1_Final(hash, &context);

    // Look for a hash that matches.
    // The substitute mapping list can (intentionally) contain multiple
    // filename mappings for the same hash. This allows us to try
    // different files and fall back if our first choice isn't found.

    filename = NULL;

    for (i = 0; i < subst_music_len; ++i)
    {
        if (memcmp(hash, subst_music[i].hash, sizeof(hash)) == 0)
        {
            filename = subst_music[i].filename;

            // If the file exists, then use this file in preference to
            // any fallbacks. But we always return a filename if it's
            // in the list, even if it's just so we can print an error
            // message to the user saying it doesn't exist.
            if (M_FileExists(filename))
            {
                break;
            }
        }
    }

    return filename;
}

// Add a substitute music file to the lookup list.

static void AddSubstituteMusic(subst_music_t *subst)
{
    ++subst_music_len;
    subst_music =
        realloc(subst_music, sizeof(subst_music_t) * subst_music_len);
    memcpy(&subst_music[subst_music_len - 1], subst, sizeof(subst_music_t));
}

static int ParseHexDigit(char c)
{
    c = tolower(c);

    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    else if (c >= 'a' && c <= 'f')
    {
        return 10 + (c - 'a');
    }
    else
    {
        return -1;
    }
}

static char *GetFullPath(char *base_filename, char *path)
{
    char *basedir, *result;
    char *p;

    // Starting with directory separator means we have an absolute path,
    // so just return it.
    if (path[0] == DIR_SEPARATOR)
    {
        return strdup(path);
    }

#ifdef _WIN32
    // d:\path\...
    if (isalpha(path[0]) && path[1] == ':' && path[2] == DIR_SEPARATOR)
    {
        return strdup(path);
    }
#endif

    // Paths in the substitute filenames can contain Unix-style /
    // path separators, but we should convert this to the separator
    // for the native platform.
    path = M_StringReplace(path, "/", DIR_SEPARATOR_S);

    // Copy config filename and cut off the filename to just get the
    // parent dir.
    basedir = strdup(base_filename);
    p = strrchr(basedir, DIR_SEPARATOR);
    if (p != NULL)
    {
        p[1] = '\0';
        result = M_StringJoin(basedir, path, NULL);
    }
    else
    {
        result = strdup(path);
    }
    free(basedir);
    free(path);

    return result;
}

// Parse a line from substitute music configuration file; returns error
// message or NULL for no error.

static char *ParseSubstituteLine(char *filename, char *line)
{
    subst_music_t subst;
    char *p;
    int hash_index;

    // Strip out comments if present.
    p = strchr(line, '#');
    if (p != NULL)
    {
        while (p > line && isspace(*(p - 1)))
        {
            --p;
        }
        *p = '\0';
    }

    // Skip leading spaces.
    for (p = line; *p != '\0' && isspace(*p); ++p);

    // Empty line? This includes comment lines now that comments have
    // been stripped.
    if (*p == '\0')
    {
        return NULL;
    }

    // Read hash.
    hash_index = 0;
    while (*p != '\0' && *p != '=' && !isspace(*p))
    {
        int d1, d2;

        d1 = ParseHexDigit(p[0]);
        d2 = ParseHexDigit(p[1]);

        if (d1 < 0 || d2 < 0)
        {
            return "Invalid hex digit in SHA1 hash";
        }
        else if (hash_index >= sizeof(sha1_digest_t))
        {
            return "SHA1 hash too long";
        }

        subst.hash[hash_index] = (d1 << 4) | d2;
        ++hash_index;

        p += 2;
    }

    if (hash_index != sizeof(sha1_digest_t))
    {
        return "SHA1 hash too short";
    }

    // Skip spaces.
    for (; *p != '\0' && isspace(*p); ++p);

    if (*p != '=')
    {
        return "Expected '='";
    }

    ++p;

    // Skip spaces.
    for (; *p != '\0' && isspace(*p); ++p);

    // We're now at the filename. Cut off trailing space characters.
    while (strlen(p) > 0 && isspace(p[strlen(p) - 1]))
    {
        p[strlen(p) - 1] = '\0';
    }

    if (strlen(p) == 0)
    {
        return "No filename specified for music substitution";
    }

    // Expand full path and add to our database of substitutes.
    subst.filename = GetFullPath(filename, p);
    AddSubstituteMusic(&subst);

    return NULL;
}

// Read a substitute music configuration file.

static boolean ReadSubstituteConfig(char *filename)
{
    char line[128];
    FILE *fs;
    char *error;
    int linenum = 1;
    int old_subst_music_len;

    fs = fopen(filename, "r");

    if (fs == NULL)
    {
        return false;
    }

    old_subst_music_len = subst_music_len;

    while (!feof(fs))
    {
        M_StringCopy(line, "", sizeof(line));
        fgets(line, sizeof(line), fs);

        error = ParseSubstituteLine(filename, line);

        if (error != NULL)
        {
            fprintf(stderr, "%s:%i: Error: %s\n", filename, linenum, error);
        }

        ++linenum;
    }

    fclose(fs);

    return true;
}

// Find substitute configs and try to load them.

static void LoadSubstituteConfigs(void)
{
    char *musicdir;
    char *path;
    unsigned int i;

    if (!strcmp(configdir, ""))
    {
        musicdir = strdup("");
    }
    else
    {
        musicdir = M_StringJoin(configdir, "music", DIR_SEPARATOR_S, NULL);
    }

    // Load all music packs. We always load all music substitution packs for
    // all games. Why? Suppose we have a Doom PWAD that reuses some music from
    // Heretic. If we have the Heretic music pack loaded, then we get an
    // automatic substitution.
    for (i = 0; i < arrlen(subst_config_filenames); ++i)
    {
        path = M_StringJoin(musicdir, subst_config_filenames[i], NULL);
        ReadSubstituteConfig(path);
        free(path);
    }

    free(musicdir);

    if (subst_music_len > 0)
    {
        printf("Loaded %i music substitutions from config files.\n",
               subst_music_len);
    }
}

// Returns true if the given lump number is a music lump that should
// be included in substitute configs.
// Identifying music lumps by name is not feasible; some games (eg.
// Heretic, Hexen) don't have a common naming pattern for music lumps.

static boolean IsMusicLump(int lumpnum)
{
    byte *data;
    boolean result;

    if (W_LumpLength(lumpnum) < 4)
    {
        return false;
    }

    data = W_CacheLumpNum(lumpnum, PU_STATIC);

    result = memcmp(data, MUS_HEADER_MAGIC, 4) == 0
          || memcmp(data, MID_HEADER_MAGIC, 4) == 0;

    W_ReleaseLumpNum(lumpnum);

    return result;
}

// Dump an example config file containing checksums for all MIDI music
// found in the WAD directory.

static void DumpSubstituteConfig(char *filename)
{
    sha1_context_t context;
    sha1_digest_t digest;
    char name[9];
    byte *data;
    FILE *fs;
    int lumpnum, h;

    fs = fopen(filename, "w");

    if (fs == NULL)
    {
        I_Error("Failed to open %s for writing", filename);
        return;
    }

    fprintf(fs, "# Example %s substitute MIDI file.\n\n", PACKAGE_NAME);
    fprintf(fs, "# SHA1 hash                              = filename\n");

    for (lumpnum = 0; lumpnum < numlumps; ++lumpnum)
    {
        strncpy(name, lumpinfo[lumpnum].name, 8);
        name[8] = '\0';

        if (!IsMusicLump(lumpnum))
        {
            continue;
        }

        // Calculate hash.
        data = W_CacheLumpNum(lumpnum, PU_STATIC);
        SHA1_Init(&context);
        SHA1_Update(&context, data, W_LumpLength(lumpnum));
        SHA1_Final(digest, &context);
        W_ReleaseLumpNum(lumpnum);

        // Print line.
        for (h = 0; h < sizeof(sha1_digest_t); ++h)
        {
            fprintf(fs, "%02x", digest[h]);
        }

        fprintf(fs, " = %s.ogg\n", name);
    }

    fprintf(fs, "\n");
    fclose(fs);

    printf("Substitute MIDI config file written to %s.\n", filename);
    I_Quit();
}

// If the temp_timidity_cfg config variable is set, generate a "wrapper"
// config file for Timidity to point to the actual config file. This
// is needed to inject a "dir" command so that the patches are read
// relative to the actual config file.

static boolean WriteWrapperTimidityConfig(char *write_path)
{
    char *p, *path;
    FILE *fstream;

    if (!strcmp(timidity_cfg_path, ""))
    {
        return false;
    }

    fstream = fopen(write_path, "w");

    if (fstream == NULL)
    {
        return false;
    }

    p = strrchr(timidity_cfg_path, DIR_SEPARATOR);
    if (p != NULL)
    {
        path = strdup(timidity_cfg_path);
        path[p - timidity_cfg_path] = '\0';
        fprintf(fstream, "dir %s\n", path);
        free(path);
    }

    fprintf(fstream, "source %s\n", timidity_cfg_path);
    fclose(fstream);

    return true;
}

void I_InitTimidityConfig(void)
{
    char *env_string;
    boolean success;

    temp_timidity_cfg = M_TempFile("timidity.cfg");

    success = WriteWrapperTimidityConfig(temp_timidity_cfg);

    // Set the TIMIDITY_CFG environment variable to point to the temporary
    // config file.

    if (success)
    {
        env_string = M_StringJoin("TIMIDITY_CFG=", temp_timidity_cfg, NULL);
        putenv(env_string);
    }
    else
    {
        free(temp_timidity_cfg);
        temp_timidity_cfg = NULL;
    }
}

// Remove the temporary config file generated by I_InitTimidityConfig().

static void RemoveTimidityConfig(void)
{
    if (temp_timidity_cfg != NULL)
    {
        remove(temp_timidity_cfg);
        free(temp_timidity_cfg);
    }
}

// Shutdown music

static void I_SDL_ShutdownMusic(void)
{
    if (music_initialized)
    {
        if (music_track != NULL)
        {
            MIX_StopTrack(music_track, 0);
            MIX_DestroyTrack(music_track);
            music_track = NULL;
        }

        music_initialized = false;

        if (sdl_was_initialized)
        {
            // Destroying a device mixer also closes the audio device.
            MIX_DestroyMixer(music_mixer);
            music_mixer = NULL;
            MIX_Quit();
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            sdl_was_initialized = false;
        }
    }
}

static boolean SDLIsInitialized(void)
{
    return music_mixer != NULL;
}

// Start (or restart) the music track, playing the given audio.
// loops uses SDL3_mixer semantics: 0 means play once, -1 means loop forever.

static boolean StartTrack(MIX_Audio *music, int loops, Sint64 start_frame)
{
    SDL_PropertiesID options;
    boolean result;

    if (music_track == NULL || !MIX_SetTrackAudio(music_track, music))
    {
        return false;
    }

    options = SDL_CreateProperties();

    if (options == 0)
    {
        return false;
    }

    SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, loops);

    if (start_frame > 0)
    {
        SDL_SetNumberProperty(options, MIX_PROP_PLAY_START_FRAME_NUMBER,
                              start_frame);
    }

    result = MIX_PlayTrack(music_track, options);
    SDL_DestroyProperties(options);

    return result;
}

// Initialize music subsystem
static boolean I_SDL_InitMusic(void)
{
    int i;

    // SDL_mixer prior to v1.2.11 has a bug that causes crashes
    // with MIDI playback.  Print a warning message if we are
    // using an old version.

#ifdef __MACOSX__
    if (MIX_Version() < SDL_VERSIONNUM(3, 0, 0))
    {
        printf("\n"
           "                   *** WARNING ***\n"
           "      You are using an old version of SDL_mixer.\n"
           "      Music playback on this version may cause crashes\n"
           "      under OS X and is disabled by default.\n"
           "\n");
    }
#endif

    //!
    // @arg <output filename>
    //
    // Read all MIDI files from loaded WAD files, dump an example substitution
    // music config file to the specified filename and quit.
    //

    i = M_CheckParmWithArgs("-dumpsubstconfig", 1);

    if (i > 0)
    {
        DumpSubstituteConfig(myargv[i + 1]);
    }

    // If SDL_mixer is not initialized, we have to initialize it
    // and have the responsibility to shut it down later on.

    if (SDLIsInitialized())
    {
        music_initialized = true;
    }
    else
    {
        const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, snd_samplerate};

        if (!SDL_Init(SDL_INIT_AUDIO))
        {
            fprintf(stderr, "Unable to set up sound.\n");
        }
        else if (!MIX_Init())
        {
            fprintf(stderr, "Error initializing SDL_mixer: %s\n",
                    SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
        else
        {
            music_mixer = MIX_CreateMixerDevice(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);

            if (music_mixer != NULL)
            {
                music_track = MIX_CreateTrack(music_mixer);
            }

            if (music_track == NULL)
            {
                fprintf(stderr, "Error initializing SDL_mixer: %s\n",
                        SDL_GetError());

                if (music_mixer != NULL)
                {
                    MIX_DestroyMixer(music_mixer);
                    music_mixer = NULL;
                }

                MIX_Quit();
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
            }
            else
            {
                sdl_was_initialized = true;
                music_initialized = true;
            }
        }
    }

    // Once initialization is complete, the temporary Timidity config
    // file can be removed.

    RemoveTimidityConfig();

    // SDL3_mixer has no equivalent of SDL2_mixer's Mix_SetMusicCMD(), so
    // external music playback programs cannot be supported.

    if (strlen(snd_musiccmd) > 0)
    {
        fprintf(stderr, "SDL3_Mixer does not support external music playback.\n");
    }

    // If we're in GENMIDI mode, try to load sound packs.
    if (snd_musicdevice == SNDDEVICE_GENMIDI)
    {
        LoadSubstituteConfigs();
    }

    return music_initialized;
}

//
// SDL_mixer's native MIDI music playing does not pause properly.
// As a workaround, set the volume to 0 when paused.
//

static void UpdateMusicVolume(void)
{
    float gain;

    if (musicpaused)
    {
        gain = 0.0f;
    }
    else
    {
        gain = (float) current_music_volume / 127.0f;
    }

    if (music_track != NULL)
    {
        MIX_SetTrackGain(music_track, gain);
    }
}

// Set music volume (0 - 127)

static void I_SDL_SetMusicVolume(int volume)
{
    // Internal state variable.
    current_music_volume = volume;

    UpdateMusicVolume();
}

// Start playing a mid

static void I_SDL_PlaySong(void *handle, boolean looping)
{
    int loops;

    if (!music_initialized)
    {
        return;
    }

    if (handle == NULL)
    {
        return;
    }

    current_track_music = (MIX_Audio *) handle;
    current_track_loop = looping;

    // SDL3_mixer counts *additional* iterations: 0 plays the track once,
    // -1 loops forever.
    if (looping)
    {
        loops = -1;
    }
    else
    {
        loops = 0;
    }

    // Don't loop when playing substitute music, as we do it
    // ourselves instead.
    if (playing_substitute && file_metadata.valid)
    {
        loops = 0;
    }

    StartTrack(current_track_music, loops, 0);
}

static void I_SDL_PauseSong(void)
{
    if (!music_initialized)
    {
        return;
    }

    musicpaused = true;

    UpdateMusicVolume();
}

static void I_SDL_ResumeSong(void)
{
    if (!music_initialized)
    {
        return;
    }

    musicpaused = false;

    UpdateMusicVolume();
}

static void I_SDL_StopSong(void)
{
    if (!music_initialized)
    {
        return;
    }

    MIX_StopTrack(music_track, 0);
    playing_substitute = false;
    current_track_music = NULL;
}

static void I_SDL_UnRegisterSong(void *handle)
{
    MIX_Audio *music = (MIX_Audio *) handle;

    if (!music_initialized)
    {
        return;
    }

    if (handle == NULL)
    {
        return;
    }

    // The track holds a reference to its audio, so make sure it is not the
    // one we are about to destroy.
    if (current_track_music == music)
    {
        MIX_StopTrack(music_track, 0);
        MIX_SetTrackAudio(music_track, NULL);
        current_track_music = NULL;
    }

    MIX_DestroyAudio(music);
}

// Determine whether memory block is a .mid file 

static boolean IsMid(byte *mem, int len)
{
    return len > 4 && !memcmp(mem, "MThd", 4);
}

static boolean ConvertMus(byte *musdata, int len, char *filename)
{
    MEMFILE *instream;
    MEMFILE *outstream;
    void *outbuf;
    size_t outbuf_len;
    int result;

    instream = mem_fopen_read(musdata, len);
    outstream = mem_fopen_write();

    result = mus2mid(instream, outstream);

    if (result == 0)
    {
        mem_get_buf(outstream, &outbuf, &outbuf_len);

        M_WriteFile(filename, outbuf, outbuf_len);
    }

    mem_fclose(instream);
    mem_fclose(outstream);

    return result;
}

static void *I_SDL_RegisterSong(void *data, int len)
{
    char *filename;
    MIX_Audio *music;

    if (!music_initialized)
    {
        return NULL;
    }

    playing_substitute = false;

    // See if we're substituting this MUS for a high-quality replacement.
    filename = GetSubstituteMusicFile(data, len);

    if (filename != NULL)
    {
        music = MIX_LoadAudio(music_mixer, filename, false);

        if (music == NULL)
        {
            // Fall through and play MIDI normally, but print an error
            // message.
            fprintf(stderr, "Failed to load substitute music file: %s: %s\n",
                    filename, SDL_GetError());
        }
        else
        {
            // Read loop point metadata from the file so that we know where
            // to loop the music.
            playing_substitute = true;
            ReadLoopPoints(filename, &file_metadata);
            return music;
        }
    }

    // MUS files begin with "MUS"
    // Reject anything which doesnt have this signature

    filename = M_TempFile("doom.mid");

    if (IsMid(data, len) && len < MAXMIDLENGTH)
    {
        M_WriteFile(filename, data, len);
    }
    else
    {
	// Assume a MUS file and try to convert

        ConvertMus(data, len, filename);
    }

    // Load the MIDI. SDL3_mixer could load this straight from an
    // SDL_IOStream with MIX_LoadAudio_IO(), but we keep writing a temporary
    // file so the on-disk behaviour below is unchanged.

    music = MIX_LoadAudio(music_mixer, filename, false);

    if (music == NULL)
    {
        // Failed to load

        fprintf(stderr, "Error loading midi: %s\n", SDL_GetError());
    }

    // Remove the temporary MIDI file; however, when using an external
    // MIDI program we can't delete the file. Otherwise, the program
    // won't find the file to play. This means we leave a mess on
    // disk :(

    if (strlen(snd_musiccmd) == 0)
    {
        remove(filename);
    }

    free(filename);

    return music;
}

// Is the song playing?
static boolean I_SDL_MusicIsPlaying(void)
{
    if (!music_initialized)
    {
        return false;
    }

    return music_track != NULL && MIX_TrackPlaying(music_track);
}

// Get position in substitute music track, in seconds since start of track.
static double GetMusicPosition(void)
{
    Sint64 frames;

    if (music_track == NULL)
    {
        return 0.0;
    }

    // SDL3_mixer tracks the playback position of the track's input for us,
    // in sample frames of the source audio.
    frames = MIX_GetTrackPlaybackPosition(music_track);

    if (frames < 0)
    {
        return 0.0;
    }

    return (double) MIX_TrackFramesToMS(music_track, frames) / 1000.0;
}

static void RestartCurrentTrack(void)
{
    // If the track is playing on loop then reset to the start point.
    // Otherwise we need to stop the track.
    if (current_track_loop)
    {
        // Restart from the loop start point. SDL3_mixer's start position is
        // given in sample frames of the source file, which is exactly the
        // unit the Vorbis loop metadata uses.
        if (current_track_music != NULL)
        {
            StartTrack(current_track_music, 0, file_metadata.start_time);
        }
    }
    else
    {
        MIX_StopTrack(music_track, 0);
        current_track_music = NULL;
        playing_substitute = false;
    }
}

// Poll music position; if we have passed the loop point end position
// then we need to go back.
static void I_SDL_PollMusic(void)
{
    if (playing_substitute && file_metadata.valid)
    {
        double end = (double) file_metadata.end_time
                   / file_metadata.samplerate_hz;

        // If we have reached the loop end point then we have to take action.
        if (file_metadata.end_time >= 0 && GetMusicPosition() >= end)
        {
            RestartCurrentTrack();
        }

        // Have we reached the actual end of track (not loop end)?
        if (!I_SDL_MusicIsPlaying() && current_track_loop)
        {
            RestartCurrentTrack();
        }
    }
}

static snddevice_t music_sdl_devices[] =
{
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

music_module_t music_sdl_module =
{
    music_sdl_devices,
    arrlen(music_sdl_devices),
    I_SDL_InitMusic,
    I_SDL_ShutdownMusic,
    I_SDL_SetMusicVolume,
    I_SDL_PauseSong,
    I_SDL_ResumeSong,
    I_SDL_RegisterSong,
    I_SDL_UnRegisterSong,
    I_SDL_PlaySong,
    I_SDL_StopSong,
    I_SDL_MusicIsPlaying,
    I_SDL_PollMusic,
};

