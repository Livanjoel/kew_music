/* kew - a command-line music player
Copyright (C) 2022 Ravachol

http://github.com/ravachol/kew

$$\
$$ |
$$ |  $$\  $$$$$$\  $$\  $$\  $$\
$$ | $$  |$$  __$$\ $$ | $$ | $$ |
$$$$$$  / $$$$$$$$ |$$ | $$ | $$ |
$$  _$$<  $$   ____|$$ | $$ | $$ |
$$ | \$$\ \$$$$$$$\ \$$$$$\$$$$  |
\__|  \__| \_______| \_____\____/

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef __USE_POSIX
#define __USE_POSIX
#endif

#ifdef __FreeBSD__
#define __BSD_VISIBLE 1
#endif

#include <dirent.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "appstate.h"
#include "cache.h"
#include "events.h"
#include "file.h"
#include "mpris.h"
#include "player.h"
#include "playerops.h"
#include "playlist.h"
#include "search_ui.h"
#include "settings.h"
#include "sound.h"
#include "soundcommon.h"
#include "songloader.h"
#include "utils.h"
#ifdef USE_LIBNOTIFY
#include "libnotify/notify.h"
#endif

// #define DEBUG 1
#define MAX_TMP_SEQ_LEN 256                     // Maximum length of temporary sequence buffer
#define COOLDOWN_MS 500
#define COOLDOWN2_MS 100

#define NUM_KEY_MAPPINGS 48

FILE *logFile = NULL;
struct winsize windowSize;
char digitsPressed[MAX_SEQ_LEN];
int digitsPressedCount = 0;
static unsigned int updateCounter = 0;
bool gPressed = false;
bool startFromTop = false;
int lastNotifiedId = -1;
bool songWasRemoved = false;
bool noPlaylist = false;
GMainLoop *main_loop;
EventMapping keyMappings[NUM_KEY_MAPPINGS];
struct timespec lastInputTime;                  // When the user last pressed a key FIXME: Should ideally only be reset in one place
bool exactSearch = false;
int fuzzySearchThreshold = 2;
int maxDigitsPressedCount = 9;

void updateLastInputTime(void)
{
        clock_gettime(CLOCK_MONOTONIC, &lastInputTime);
}

bool isCooldownElapsed(int milliSeconds)
{
        struct timespec currentTime;
        clock_gettime(CLOCK_MONOTONIC, &currentTime);
        double elapsedMilliseconds = (currentTime.tv_sec - lastInputTime.tv_sec) * 1000.0 +
                                     (currentTime.tv_nsec - lastInputTime.tv_nsec) / 1000000.0;

        return elapsedMilliseconds >= milliSeconds;
}

struct Event processInput()
{
        struct Event event;
        event.type = EVENT_NONE;
        event.key[0] = '\0';
        bool cooldownElapsed = false;
        bool cooldown2Elapsed = false;

        if (!isInputAvailable())
        {
                flushSeek();
                return event;
        }

        if (isCooldownElapsed(COOLDOWN_MS))
                cooldownElapsed = true;

        if (isCooldownElapsed(COOLDOWN2_MS))
                cooldown2Elapsed = true;

        int seqLength = 0;
        char seq[MAX_SEQ_LEN];
        seq[0] = '\0'; // Set initial value
        int keyReleased = 0;

        // Find input
        while (isInputAvailable())
        {
                char tmpSeq[MAX_TMP_SEQ_LEN];

                seqLength = seqLength + readInputSequence(tmpSeq, sizeof(tmpSeq));

                // Release most keys directly, seekbackward and seekforward can be read continuously
                if (seqLength <= 0 && strcmp(seq + 1, settings.seekBackward) != 0 && strcmp(seq + 1, settings.seekForward) != 0)
                {
                        keyReleased = 1;
                        break;
                }

                if (strlen(seq) + strlen(tmpSeq) >= MAX_SEQ_LEN)
                {
                        break;
                }

                strcat(seq, tmpSeq);

                // This slows the continous reads down to not get a a too fast scrolling speed
                if (strcmp(seq + 1, settings.hardScrollUp) == 0 || strcmp(seq + 1, settings.hardScrollDown) == 0 || strcmp(seq + 1, settings.scrollUpAlt) == 0 ||
                    strcmp(seq + 1, settings.scrollDownAlt) == 0 || strcmp(seq + 1, settings.seekBackward) == 0 || strcmp(seq + 1, settings.seekForward) == 0 ||
                    strcmp(seq + 1, settings.hardNextPage) == 0 || strcmp(seq + 1, settings.hardPrevPage) == 0)
                {
                        keyReleased = 0;
                        readInputSequence(tmpSeq, sizeof(tmpSeq)); // dummy read to prevent scrolling after key released
                        break;
                }

                keyReleased = 0;
        }

        if (keyReleased)
                return event;

        event.type = EVENT_NONE;

        strncpy(event.key, seq, MAX_SEQ_LEN);

        if (appState.currentView == SEARCH_VIEW)
        {
                if (strcmp(event.key, "\x7F") == 0 || strcmp(event.key, "\x08") == 0)
                {
                        removeFromSearchText();
                        resetSearchResult();
                        fuzzySearch(getLibrary(), fuzzySearchThreshold);
                        event.type = EVENT_SEARCH;
                }
                else if (((strlen(event.key) == 1 && event.key[0] != '\033' && event.key[0] != '\n' && event.key[0] != '\t' && event.key[0] != '\r') || strcmp(event.key, " ") == 0 || (unsigned char)event.key[0] >= 0xC0) && strcmp(event.key, "Z") != 0 && strcmp(event.key, "X") != 0 && strcmp(event.key, "C") != 0 && strcmp(event.key, "V") != 0 && strcmp(event.key, "B") != 0)
                {
                        addToSearchText(event.key);
                        resetSearchResult();
                        fuzzySearch(getLibrary(), fuzzySearchThreshold);
                        event.type = EVENT_SEARCH;
                }
        }

        // Set event for pressed key
        for (int i = 0; i < NUM_KEY_MAPPINGS; i++)
        {
                if (keyMappings[i].seq[0] != '\0' &&
                    ((seq[0] == '\033' && strlen(seq) > 1 && strcmp(seq + 1, keyMappings[i].seq) == 0) ||
                     strcmp(seq, keyMappings[i].seq) == 0))
                {
                        if (event.type == EVENT_SEARCH && keyMappings[i].eventType != EVENT_GOTOSONG)
                        {
                                break;
                        }

                        event.type = keyMappings[i].eventType;
                        break;
                }
        }

        // Handle gg
        if (event.key[0] == 'g' && event.type == EVENT_NONE)
        {
                if (gPressed)
                {
                        event.type = EVENT_GOTOBEGINNINGOFPLAYLIST;
                        gPressed = false;
                }
                else
                {
                        gPressed = true;
                }
        }

        // Handle numbers
        if (isdigit(event.key[0]))
        {
                if (digitsPressedCount < maxDigitsPressedCount)
                        digitsPressed[digitsPressedCount++] = event.key[0];
        }
        else
        {
                // Handle multiple digits, sometimes mixed with other keys
                for (int i = 0; i < MAX_SEQ_LEN; i++)
                {
                        if (isdigit(seq[i]))
                        {
                                if (digitsPressedCount < maxDigitsPressedCount)
                                        digitsPressed[digitsPressedCount++] = seq[i];
                        }
                        else
                        {
                                if (seq[i] == '\0')
                                        break;

                                if (seq[i] != settings.switchNumberedSong[0] &&
                                    seq[i] != settings.hardSwitchNumberedSong[0] &&
                                    seq[i] != settings.hardEndOfPlaylist[0])
                                {
                                        memset(digitsPressed, '\0', sizeof(digitsPressed));
                                        digitsPressedCount = 0;
                                        break;
                                }
                                else if (seq[i] == settings.hardEndOfPlaylist[0])
                                {
                                        event.type = EVENT_GOTOENDOFPLAYLIST;
                                        break;
                                }
                                else
                                {
                                        event.type = EVENT_GOTOSONG;
                                        break;
                                }
                        }
                }
        }

        // Handle song prev/next cooldown
        if (!cooldownElapsed && (event.type == EVENT_NEXT || event.type == EVENT_PREV))
                event.type = EVENT_NONE;
        else if (event.type == EVENT_NEXT || event.type == EVENT_PREV)
                updateLastInputTime();

        // Handle seek/remove cooldown
        if (!cooldown2Elapsed && (event.type == EVENT_REMOVE || event.type == EVENT_SEEKBACK || event.type == EVENT_SEEKFORWARD))
                event.type = EVENT_NONE;
        else if (event.type == EVENT_REMOVE || event.type == EVENT_SEEKBACK || event.type == EVENT_SEEKFORWARD)
                updateLastInputTime();

        // Forget Numbers
        if (event.type != EVENT_GOTOSONG && event.type != EVENT_GOTOENDOFPLAYLIST && event.type != EVENT_NONE)
        {
                memset(digitsPressed, '\0', sizeof(digitsPressed));
                digitsPressedCount = 0;
        }

        // Forget g pressed
        if (event.key[0] != 'g')
        {
                gPressed = false;
        }

        return event;
}

void setEndOfListReached(AppState *state)
{
        state->currentView = LIBRARY_VIEW;
        loadedNextSong = false;
        audioData.endOfListReached = true;
        usingSongDataA = false;
        currentSong = NULL;
        audioData.currentFileIndex = 0;
        audioData.restart = true;
        loadingdata.loadA = true;
        emitMetadataChanged("", "", "", "", "/org/mpris/MediaPlayer2/TrackList/NoTrack", NULL, 0);
        emitPlaybackStoppedMpris();
        pthread_mutex_lock(&dataSourceMutex);
        cleanupPlaybackDevice();
        pthread_mutex_unlock(&dataSourceMutex);
        refresh = true;
}

void notifyMPRISSwitch(SongData *currentSongData)
{
        if (currentSongData == NULL)
                return;

        gint64 length = getLengthInMicroSec(currentSongData->duration);

        // update mpris
        emitMetadataChanged(
            currentSongData->metadata->title,
            currentSongData->metadata->artist,
            currentSongData->metadata->album,
            currentSongData->coverArtPath,
            currentSongData->trackId != NULL ? currentSongData->trackId : "", currentSong,
            length);
}

void notifySongSwitch(SongData *currentSongData, UISettings *ui)
{
        if (currentSongData != NULL && currentSongData->hasErrors == 0 && currentSongData->metadata && strlen(currentSongData->metadata->title) > 0)
        {
#ifdef USE_LIBNOTIFY
                displaySongNotification(currentSongData->metadata->artist, currentSongData->metadata->title, currentSongData->coverArtPath, ui);

#elif __APPLE__
                displaySongNotificationApple(currentSongData->metadata->artist, currentSongData->metadata->title, currentSongData->coverArtPath, ui);
#endif

                notifyMPRISSwitch(currentSongData);

                lastNotifiedId = currentSong->id;
        }
}

void determineSongAndNotify(UISettings *ui)
{
        SongData *currentSongData = NULL;

        bool isDeleted = determineCurrentSongData(&currentSongData);

        if (lastNotifiedId != currentSong->id)
        {
                if (!isDeleted)
                        notifySongSwitch(currentSongData, ui);
        }
}

// Checks conditions for refreshing player
bool shouldRefreshPlayer()
{
        return !skipping && !isEOFReached() && !isImplSwitchReached();
}

// Refreshes the player visually if conditions are met
void refreshPlayer(UIState *uis)
{
        int mutexResult = pthread_mutex_trylock(&switchMutex);

        if (mutexResult != 0)
        {
                fprintf(stderr, "Failed to lock switch mutex.\n");
                return;
        }

        if (uis->doNotifyMPRISPlaying)
        {
                uis->doNotifyMPRISPlaying = false;
                
                emitStringPropertyChanged("PlaybackStatus", "Playing");
        }

        if (uis->doNotifyMPRISSwitched)
        {
                uis->doNotifyMPRISSwitched = false;

                notifyMPRISSwitch(getCurrentSongData());
        }

        if (shouldRefreshPlayer())
        {
                printPlayer(getCurrentSongData(), elapsedSeconds, &settings, &appState);
        }

        pthread_mutex_unlock(&switchMutex);
}

void resetListAfterDequeuingPlayingSong(AppState *state)
{
        if (lastPlayedId < 0)
                return;

        Node *node = findSelectedEntryById(&playlist, lastPlayedId);

        if (currentSong == NULL && node == NULL)
        {
                stopPlayback();

                loadedNextSong = false;
                audioData.endOfListReached = true;
                audioData.restart = true;

                emitMetadataChanged("", "", "", "", "/org/mpris/MediaPlayer2/TrackList/NoTrack", NULL, 0);
                emitPlaybackStoppedMpris();

                pthread_mutex_lock(&dataSourceMutex);
                cleanupPlaybackDevice();
                pthread_mutex_unlock(&dataSourceMutex);

                refresh = true;

                switchAudioImplementation();

                unloadSongA(state);
                unloadSongB(state);
                songWasRemoved = true;
                userData.currentSongData = NULL;

                audioData.currentFileIndex = 0;
                audioData.restart = true;
                waitingForNext = true;
                startFromTop = true;
                loadingdata.loadA = true;
                usingSongDataA = false;

                ma_data_source_uninit(&audioData);

                audioData.switchFiles = false;

                if (playlist.count == 0)
                        songToStartFrom = NULL;
        }
}

void handleGoToSong(AppState *state)
{
        bool canGoNext = (currentSong != NULL && currentSong->next != NULL);

        if (state->currentView == LIBRARY_VIEW)
        {
                if (audioData.restart)
                {
                        Node *lastSong = findSelectedEntryById(&playlist, lastPlayedId);
                        startFromTop = false;

                        if (lastSong == NULL)
                        {
                                if (playlist.tail != NULL)
                                        lastPlayedId = playlist.tail->id;
                                else
                                {
                                        lastPlayedId = -1;
                                        startFromTop = true;
                                }
                        }
                }

                pthread_mutex_lock(&(playlist.mutex));

                enqueueSongs(getCurrentLibEntry(), &state->uiState);
                resetListAfterDequeuingPlayingSong(state);

                pthread_mutex_unlock(&(playlist.mutex));
        }
        else if (state->currentView == SEARCH_VIEW)
        {
                pthread_mutex_lock(&(playlist.mutex));

                FileSystemEntry *entry = getCurrentSearchEntry();

                setChosenDir(entry);

                enqueueSongs(entry, &state->uiState);

                resetListAfterDequeuingPlayingSong(state);

                pthread_mutex_unlock(&(playlist.mutex));
        }
        else
        {
                if (digitsPressedCount == 0)
                {
                        if (isPaused() && currentSong != NULL && state->uiState.chosenNodeId == currentSong->id)
                        {
                                togglePause(&totalPauseSeconds, &pauseSeconds, &pause_time);
                        }
                        else
                        {
                                loadedNextSong = true;

                                nextSongNeedsRebuilding = false;

                                unloadSongA(state);
                                unloadSongB(state);

                                usingSongDataA = false;
                                audioData.currentFileIndex = 0;
                                loadingdata.loadA = true;

                                bool wasEndOfList = false;
                                if (audioData.endOfListReached)
                                        wasEndOfList = true;

                                skipToSong(state->uiState.chosenNodeId, true);

                                if ((songWasRemoved && currentSong != NULL))
                                {
                                        usingSongDataA = !usingSongDataA;
                                        songWasRemoved = false;
                                }

                                if (wasEndOfList)
                                        usingSongDataA = true;

                                audioData.endOfListReached = false;
                        }
                }
                else
                {
                        state->uiState.resetPlaylistDisplay = true;
                        int songNumber = atoi(digitsPressed);
                        memset(digitsPressed, '\0', sizeof(digitsPressed));
                        digitsPressedCount = 0;

                        nextSongNeedsRebuilding = false;

                        skipToNumberedSong(songNumber);
                }
        }

        // Handle MPRIS CanGoNext
        bool couldGoNext = (currentSong != NULL && currentSong->next != NULL);
        if (canGoNext != couldGoNext) {
                emitBooleanPropertyChanged("CanGoNext", couldGoNext);
        }
}

void gotoBeginningOfPlaylist(AppState *state)
{
        digitsPressed[0] = 1;
        digitsPressed[1] = '\0';
        digitsPressedCount = 1;
        handleGoToSong(state);
}

void gotoEndOfPlaylist(AppState *state)
{
        if (digitsPressedCount > 0)
        {
                handleGoToSong(state);
        }
        else
        {
                skipToLastSong();
        }
}

void handleInput(AppState *state)
{
        struct Event event = processInput();

        switch (event.type)
        {
        case EVENT_GOTOBEGINNINGOFPLAYLIST:
                gotoBeginningOfPlaylist(state);
                break;
        case EVENT_GOTOENDOFPLAYLIST:
                gotoEndOfPlaylist(state);
                break;
        case EVENT_GOTOSONG:
                handleGoToSong(state);
                break;
        case EVENT_PLAY_PAUSE:
                togglePause(&totalPauseSeconds, &pauseSeconds, &pause_time);
                break;
        case EVENT_TOGGLEVISUALIZER:
                toggleVisualizer(&settings, &(state->uiSettings));
                break;
        case EVENT_TOGGLEREPEAT:
                toggleRepeat();
                break;
        case EVENT_TOGGLEBLOCKS:
                toggleBlocks(&settings, &(state->uiSettings));
                break;
        case EVENT_SHUFFLE:
                toggleShuffle();
                emitShuffleChanged();
                break;
        case EVENT_TOGGLEPROFILECOLORS:
                toggleColors(&settings, &(state->uiSettings));
                break;
        case EVENT_QUIT:
                quit();
                break;
        case EVENT_SCROLLNEXT:
                scrollNext();
                break;
        case EVENT_SCROLLPREV:
                scrollPrev();
                break;
        case EVENT_VOLUME_UP:
                adjustVolumePercent(5);
                emitVolumeChanged();
                break;
        case EVENT_VOLUME_DOWN:
                adjustVolumePercent(-5);
                emitVolumeChanged();
                break;
        case EVENT_NEXT:
                state->uiState.resetPlaylistDisplay = true;
                skipToNextSong(state);
                break;
        case EVENT_PREV:
                state->uiState.resetPlaylistDisplay = true;
                skipToPrevSong(state);
                break;
        case EVENT_SEEKBACK:
                seekBack(&(state->uiState));
                break;
        case EVENT_SEEKFORWARD:
                seekForward(&(state->uiState));
                break;
        case EVENT_ADDTOMAINPLAYLIST:
                addToSpecialPlaylist();
                break;
        case EVENT_EXPORTPLAYLIST:
                savePlaylist(settings.path);
                break;
        case EVENT_UPDATELIBRARY:
                updateLibrary(settings.path);
                break;
        case EVENT_SHOWKEYBINDINGS:
                toggleShowKeyBindings();
                break;
        case EVENT_SHOWPLAYLIST:
                toggleShowPlaylist();
                break;
        case EVENT_SHOWSEARCH:
                toggleShowSearch();
                break;
        case EVENT_SHOWLIBRARY:
                toggleShowLibrary();
                break;
        case EVENT_NEXTPAGE:
                flipNextPage();
                break;
        case EVENT_PREVPAGE:
                flipPrevPage();
                break;
        case EVENT_REMOVE:
                handleRemove();
                resetListAfterDequeuingPlayingSong(state);
                break;
        case EVENT_SHOWTRACK:
                showTrack();
                break;
        case EVENT_TABNEXT:
                tabNext();
                break;
        default:
                fastForwarding = false;
                rewinding = false;
                break;
        }
}

void resize(UIState *uis)
{
        alarm(1); // Timer
        while (uis->resizeFlag)
        {
                uis->resizeFlag = 0;
                c_sleep(100);
        }
        alarm(0); // Cancel timer
        printf("\033[1;1H");
        clearScreen();
        refresh = true;
}

void updatePlayer(UIState *uis)
{
        struct winsize ws;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);

        // check if window has changed size
        if (ws.ws_col != windowSize.ws_col || ws.ws_row != windowSize.ws_row)
        {
                uis->resizeFlag = 1;
                windowSize = ws;
        }

        // resizeFlag can also be set by handleResize
        if (uis->resizeFlag)
                resize(uis);
        else
        {
                refreshPlayer(uis);
        }
}

void loadAudioData(AppState *state)
{
        if (audioData.restart == true)
        {
                if (playlist.head != NULL && (waitingForPlaylist || waitingForNext))
                {
                        songLoading = true;

                        if (waitingForPlaylist)
                        {
                                currentSong = playlist.head;
                        }
                        else if (waitingForNext)
                        {
                                if (songToStartFrom != NULL)
                                {
                                        // Make sure it still exists in the playlist
                                        findNodeInList(&playlist, songToStartFrom->id, &currentSong);

                                        songToStartFrom = NULL;
                                }
                                else if (lastPlayedId >= 0)
                                {
                                        currentSong = findSelectedEntryById(&playlist, lastPlayedId);
                                        if (currentSong != NULL && currentSong->next != NULL)
                                                currentSong = currentSong->next;
                                }

                                if (currentSong == NULL)
                                {
                                        if (startFromTop)
                                        {
                                                currentSong = playlist.head;
                                                startFromTop = false;
                                        }
                                        else
                                                currentSong = playlist.tail;
                                }
                        }
                        audioData.restart = false;
                        waitingForPlaylist = false;
                        waitingForNext = false;
                        songWasRemoved = false;

                        if (isShuffleEnabled())
                                reshufflePlaylist();

                        unloadSongA(state);
                        unloadSongB(state);

                        int res = loadFirst(currentSong, state);

                        finishLoading();

                        if (res >= 0)
                        {
                                res = createAudioDevice();
                        }

                        if (res >= 0)
                        {
                                resumePlayback();
                        }
                        else
                        {
                                setEndOfListReached(state);
                        }

                        loadedNextSong = false;
                        nextSong = NULL;
                        refresh = true;

                        clock_gettime(CLOCK_MONOTONIC, &start_time);
                }
        }
        else if (currentSong != NULL && (nextSongNeedsRebuilding || nextSong == NULL) && !songLoading)
        {
                loadNextSong();
                determineSongAndNotify(&state->uiSettings);
        }
}

void tryLoadNext()
{
        songHasErrors = false;
        clearingErrors = true;

        if (tryNextSong == NULL && currentSong != NULL)
                tryNextSong = currentSong->next;
        else if (tryNextSong != NULL)
                tryNextSong = tryNextSong->next;

        if (tryNextSong != NULL)
        {
                songLoading = true;
                loadingdata.loadA = !usingSongDataA;
                loadingdata.loadingFirstDecoder = false;
                loadSong(tryNextSong, &loadingdata);
        }
        else
        {
                clearingErrors = false;
        }
}

void prepareNextSong(AppState *state)
{
        if (!skipOutOfOrder && !isRepeatEnabled())
        {
                setCurrentSongToNext();
        }
        else
        {
                skipOutOfOrder = false;
        }

        finishLoading();
        resetTimeCount();

        nextSong = NULL;
        refresh = true;

        if (!isRepeatEnabled() || currentSong == NULL)
        {
                unloadPreviousSong(state);
        }

        if (currentSong == NULL)
        {
                if (state->uiSettings.quitAfterStopping)
                        quit();
                else
                        setEndOfListReached(state);
        }
        else
        {
                determineSongAndNotify(&(state->uiSettings));
        }

        clock_gettime(CLOCK_MONOTONIC, &start_time);
}

void handleSkipFromStopped()
{
        // If we don't do this the song gets loaded in the wrong slot
        if (skipFromStopped)
        {
                usingSongDataA = !usingSongDataA;
                skipOutOfOrder = false;
                skipFromStopped = false;
        }
}

gboolean mainloop_callback(gpointer data)
{
        (void)data;

        calcElapsedTime();

        handleInput(&appState);

        updateCounter++;

        // Update every other time or if searching (search needs to update often to detect keypresses)
        if (updateCounter % 2 == 0 || appState.currentView == SEARCH_VIEW)
        {
                // Process GDBus events in the global_main_context
                while (g_main_context_pending(global_main_context))
                {
                        g_main_context_iteration(global_main_context, FALSE);
                }

                updatePlayer(&appState.uiState);

                if (playlist.head != NULL)
                {
                        if ((skipFromStopped || !loadedNextSong || nextSongNeedsRebuilding) && !audioData.endOfListReached)
                        {
                                loadAudioData(&appState);
                        }

                        if (songHasErrors)
                                tryLoadNext();

                        if (isPlaybackDone())
                        {
                                updateLastSongSwitchTime();
                                prepareNextSong(&appState);
                                switchAudioImplementation();
                        }
                }
                else
                {
                        setEOFNotReached();
                }
        }
        return TRUE;
}

static gboolean quitOnSignal(gpointer user_data)
{        
        GMainLoop *loop = (GMainLoop *)user_data;
        g_main_loop_quit(loop);
        quit();
        return G_SOURCE_REMOVE; // Remove the signal source
}

void initFirstPlay(Node *song, AppState *state)
{
        updateLastInputTime();
        updateLastSongSwitchTime();

        userData.currentSongData = NULL;
        userData.songdataA = NULL;
        userData.songdataB = NULL;
        userData.songdataADeleted = true;
        userData.songdataBDeleted = true;

        int res = 0;

        if (song != NULL)
        {
                audioData.currentFileIndex = 0;
                loadingdata.loadA = true;
                res = loadFirst(song, state);

                if (res >= 0)
                {
                        res = createAudioDevice();
                }
                if (res >= 0)
                {
                        resumePlayback();
                }

                if (res < 0)
                        setEndOfListReached(state);
        }

        if (song == NULL || res < 0)
        {
                song = NULL;
                waitingForPlaylist = true;
        }

        loadedNextSong = false;
        nextSong = NULL;
        refresh = true;

        clock_gettime(CLOCK_MONOTONIC, &start_time);

        main_loop = g_main_loop_new(NULL, FALSE);

        g_unix_signal_add(SIGINT, quitOnSignal, main_loop);
        g_unix_signal_add(SIGHUP, quitOnSignal, main_loop);

        if (song != NULL)
                emitStartPlayingMpris();
        else
                emitPlaybackStoppedMpris();

        g_timeout_add(50, mainloop_callback, NULL);
        g_main_loop_run(main_loop);
        g_main_loop_unref(main_loop);
}

void cleanupOnExit()
{
        pthread_mutex_lock(&dataSourceMutex);
        resetDecoders();
        resetVorbisDecoders();
        resetOpusDecoders();
#ifdef USE_FAAD
        resetM4aDecoders();
#endif
        if (isContextInitialized)
        {
                cleanupPlaybackDevice();
                cleanupAudioContext();
        }
        emitPlaybackStoppedMpris();

        bool noMusicFound = false;

        if (library == NULL || library->children == NULL)
        {
                noMusicFound = true;
        }

        if (!userData.songdataADeleted)
        {
                userData.songdataADeleted = true;
                unloadSongData(&loadingdata.songdataA, &appState);
        }
        if (!userData.songdataBDeleted)
        {
                userData.songdataBDeleted = true;
                unloadSongData(&loadingdata.songdataB, &appState);
        }

        freeSearchResults();
        cleanupMpris();
        restoreTerminalMode();
        enableInputBuffering();
        setConfig(&settings, &(appState.uiSettings));
        saveSpecialPlaylist(settings.path);
        freeAudioBuffer();
        deleteCache(appState.tempCache);
        freeMainDirectoryTree(&appState);
        deletePlaylist(&playlist);
        deletePlaylist(originalPlaylist);
        deletePlaylist(specialPlaylist);
        free(specialPlaylist);
        free(originalPlaylist);
        setDefaultTextColor();
        pthread_mutex_destroy(&(loadingdata.mutex));
        pthread_mutex_destroy(&(playlist.mutex));
        pthread_mutex_destroy(&(switchMutex));
        pthread_mutex_unlock(&dataSourceMutex);
        pthread_mutex_destroy(&(dataSourceMutex));
#ifdef USE_LIBNOTIFY
        notify_uninit();
        cleanupPreviousNotification();
#endif
        resetConsole();
        showCursor();
        fflush(stdout);

        if (noMusicFound)
        {
                printf("No Music found.\n");
                printf("Please make sure the path is set correctly. \n");
                printf("To set it type: kew path \"/path/to/Music\". \n");
        }
        else if (noPlaylist)
        {
                printf("Music not found.\n");
        }

#ifdef DEBUG
        fclose(logFile);
#endif
        if (freopen("/dev/stderr", "w", stderr) == NULL)
        {
                perror("freopen error");
        }
}

void run(AppState *state)
{
        if (originalPlaylist == NULL)
        {

                originalPlaylist = malloc(sizeof(PlayList));
                *originalPlaylist = deepCopyPlayList(&playlist);
        }

        if (playlist.head == NULL)
        {
                state->currentView = LIBRARY_VIEW;
        }

        initMpris();

        currentSong = playlist.head;
        initFirstPlay(currentSong, state);
        clearScreen();
        fflush(stdout);
}

void handleResize(int sig)
{
        (void)sig;
        appState.uiState.resizeFlag = 1;
}

void resetResizeFlag(int sig)
{
        (void)sig;
        appState.uiState.resizeFlag = 0;
}

void initResize()
{
        signal(SIGWINCH, handleResize);

        struct sigaction sa;
        sa.sa_handler = resetResizeFlag;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGALRM, &sa, NULL);
}

void init(AppState *state)
{
        disableInputBuffering();
        srand(time(NULL));
        initResize();
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &windowSize);
        enableScrolling();
        setNonblockingMode();
        state->tempCache = createCache();
        c_strcpy(loadingdata.filePath, sizeof(loadingdata.filePath), "");
        loadingdata.songdataA = NULL;
        loadingdata.songdataB = NULL;
        loadingdata.loadA = true;
        loadingdata.loadingFirstDecoder = true;
        audioData.restart = true;
        userData.songdataADeleted = true;
        userData.songdataBDeleted = true;
        initAudioBuffer();
        initVisuals();
        pthread_mutex_init(&dataSourceMutex, NULL);
        pthread_mutex_init(&switchMutex, NULL);
        pthread_mutex_init(&(loadingdata.mutex), NULL);
        pthread_mutex_init(&(playlist.mutex), NULL);
        state->uiSettings.nerdFontsEnabled = true;
        createLibrary(&settings, state);
        setlocale(LC_ALL, "");
        fflush(stdout);
#ifdef USE_LIBNOTIFY
        notify_init("kew");
#endif

#ifdef DEBUG
        g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
        logFile = freopen("error.log", "w", stderr);
        if (logFile == NULL)
        {
                fprintf(stdout, "Failed to redirect stderr to error.log\n");
        }
#else
        FILE *nullStream = freopen("/dev/null", "w", stderr);
        (void)nullStream;
#endif
}

void openLibrary(AppState *state)
{
        state->currentView = LIBRARY_VIEW;
        init(state);
        playlist.head = NULL;
        run(state);
}

void playSpecialPlaylist(AppState *state)
{
        if (specialPlaylist->count == 0)
        {
                printf("Couldn't find any songs in the special playlist. Add a song by pressing '.' while it's playing. \n");
                exit(0);
        }

        init(state);
        deepCopyPlayListOntoList(specialPlaylist, &playlist);
        shufflePlaylist(&playlist);
        run(state);
}

void playAll(AppState *state)
{
        init(state);
        createPlayListFromFileSystemEntry(library, &playlist, MAX_FILES);
        if (playlist.count == 0)
        {
                exit(0);
        }
        shufflePlaylist(&playlist);
        run(state);
}

void playAllAlbums(AppState *state)
{
        init(state);
        addShuffledAlbumsToPlayList(library, &playlist, MAX_FILES);
        if (playlist.count == 0)
        {
                exit(0);
        }
        run(state);
}

void removeArgElement(char *argv[], int index, int *argc)
{
        if (index < 0 || index >= *argc)
        {
                // Invalid index
                return;
        }

        // Shift elements after the index
        for (int i = index; i < *argc - 1; i++)
        {
                argv[i] = argv[i + 1];
        }

        // Update the argument count
        (*argc)--;
}

void handleOptions(int *argc, char *argv[], UISettings *ui)
{
        const char *noUiOption = "--noui";
        const char *noCoverOption = "--nocover";
        const char *quitOnStop = "--quitonstop";
        const char *quitOnStop2 = "-q";
        const char *exactOption = "--exact";
        const char *exactOption2 = "-e";

        int idx = -1;
        for (int i = 0; i < *argc; i++)
        {
                if (c_strcasestr(argv[i], noUiOption))
                {
                        ui->uiEnabled = false;
                        idx = i;
                }
        }
        if (idx >= 0)
                removeArgElement(argv, idx, argc);

        idx = -1;
        for (int i = 0; i < *argc; i++)
        {
                if (c_strcasestr(argv[i], noCoverOption))
                {
                        ui->coverEnabled = false;
                        idx = i;
                }
        }
        if (idx >= 0)
                removeArgElement(argv, idx, argc);

        idx = -1;
        for (int i = 0; i < *argc; i++)
        {
                if (c_strcasestr(argv[i], quitOnStop) || c_strcasestr(argv[i], quitOnStop2))
                {
                        ui->quitAfterStopping = true;
                        idx = i;
                }
        }
        if (idx >= 0)
                removeArgElement(argv, idx, argc);

        idx = -1;
        for (int i = 0; i < *argc; i++)
        {
                if (c_strcasestr(argv[i], exactOption) || c_strcasestr(argv[i], exactOption2))
                {
                        exactSearch = true;
                        idx = i;
                }
        }
        if (idx >= 0)
                removeArgElement(argv, idx, argc);
}

#define PIDFILE_TEMPLATE "/tmp/kew_%d.pid" // Template for user-specific PID file

int isProcessRunning(pid_t pid)
{
        char proc_path[64];
        snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
        struct stat statbuf;
        return (stat(proc_path, &statbuf) == 0);
}

// Ensures only a single instance of kew can run at a time for the current user.
void exitIfAlreadyRunning()
{
        char pidfile_path[256];
        snprintf(pidfile_path, sizeof(pidfile_path), PIDFILE_TEMPLATE, getuid());

        FILE *pidfile;
        pid_t pid;

        pidfile = fopen(pidfile_path, "r");
        if (pidfile != NULL)
        {
                if (fscanf(pidfile, "%d", &pid) == 1)
                {
                        fclose(pidfile);
                        if (isProcessRunning(pid))
                        {
                                fprintf(stderr, "An instance of kew is already running. Pid: %d.\n", pid);
                                exit(EXIT_FAILURE);
                        }
                        else
                        {
                                unlink(pidfile_path);
                        }
                }
                else
                {
                        fclose(pidfile);
                        unlink(pidfile_path);
                }
        }

        // Create a new PID file
        pidfile = fopen(pidfile_path, "w");
        if (pidfile == NULL)
        {
                perror("Unable to create PID file");
                exit(EXIT_FAILURE);
        }
        fprintf(pidfile, "%d\n", getpid());
        fclose(pidfile);
}

int directoryExists(const char *path)
{
        struct stat info;
        if (stat(path, &info) != 0)
        {
                return 0;
        }
        else if (S_ISDIR(info.st_mode))
        {
                return 1;
        }

        return 0;
}

void setMusicPath()
{
        char *user = getenv("USER");

        // Fallback if USER is not set
        if (!user)
        {
                user = getlogin();

                if (!user)
                {
                        struct passwd *pw = getpwuid(getuid());
                        if (pw)
                        {
                                user = pw->pw_name;
                        }
                        else
                        {
                                printf("Error: Could not retrieve user information.\n");
                                printf("Please set a path to your music library.\n");
                                printf("To set it, type: kew path \"/path/to/Music\".\n");
                                exit(0);
                        }
                }
        }

        // Music folder names in different languages
        const char *musicFolderNames[] = {
            "Music", "Música", "Musique", "Musik", "Musica", "Muziek", "Музыка",
            "音乐", "音楽", "음악", "موسيقى", "संगीत", "Müzik", "Musikk", "Μουσική",
            "Muzyka", "Hudba", "Musiikki", "Zene", "Muzică", "เพลง", "מוזיקה"};

        char path[PATH_MAX];
        int found = 0;
        char choice = ' ';
        int result = -1;

        for (size_t i = 0; i < sizeof(musicFolderNames) / sizeof(musicFolderNames[0]); i++)
        {
#ifdef __APPLE__
                snprintf(path, sizeof(path), "/Users/%s/%s", user, musicFolderNames[i]);
#else
                snprintf(path, sizeof(path), "/home/%s/%s", user, musicFolderNames[i]);
#endif

                if (directoryExists(path))
                {
                        found = 1;
                        printf("Do you want to use %s as your music library folder?\n", path);
                        printf("y = Yes\nn = Enter a path\n");

                        result = scanf(" %c", &choice);

                        if (choice == 'y' || choice == 'Y')
                        {
                                strncpy(settings.path, path, sizeof(settings.path));
                                return;
                        }
                        else if (choice == 'n' || choice == 'N')
                        {
                                break;
                        }
                        else
                        {
                                printf("Invalid choice. Please try again.\n");
                                i--;
                        }
                }
        }

        if (!found || (found && (choice == 'n' || choice == 'N')))
        {
                printf("Please enter the path to your music library (/path/to/Music):\n");
                result = scanf("%s", path);

                if (directoryExists(path))
                {
                        strncpy(settings.path, path, sizeof(settings.path));
                }
                else
                {
                        printf("The entered path does not exist.\n");
                        exit(1);
                }
        }

        if (result == -1)
                exit(1);
}

void initState(AppState *state)
{
        state->uiSettings.nerdFontsEnabled = true;
        state->uiSettings.visualizerEnabled = true;
        state->uiSettings.coverEnabled = true;
        state->uiSettings.hideLogo = false;
        state->uiSettings.hideHelp = false;
        state->uiSettings.quitAfterStopping = false;
        state->uiSettings.coverAnsi = false;
        state->uiSettings.uiEnabled = true;
        state->uiSettings.visualizerHeight = 5;
        state->uiSettings.cacheLibrary = -1;
        state->uiSettings.useConfigColors = false;
        state->uiSettings.color.r = 125;
        state->uiSettings.color.g = 125;
        state->uiSettings.color.b = 125;
        state->uiState.numDirectoryTreeEntries = 0;
        state->uiState.numProgressBars = 35;
        state->uiState.chosenNodeId = 0;
        state->uiState.resetPlaylistDisplay = true;
        state->uiState.allowChooseSongs = false;
        state->uiState.resizeFlag = 0;
        state->uiState.doNotifyMPRISSwitched = false;
        state->uiState.doNotifyMPRISPlaying = false;
        state->tempCache = NULL;
}

int main(int argc, char *argv[])
{
        UISettings *ui = &(appState.uiSettings);

        exitIfAlreadyRunning();

        if ((argc == 2 && ((strcmp(argv[1], "--help") == 0) || (strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "-?") == 0))))
        {
                showHelp();
                exit(0);
        }
        else if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0))
        {
                printAbout(NULL, ui);
                exit(0);
        }

        initState(&appState);
        getConfig(&settings, ui);
        mapSettingsToKeys(&settings, keyMappings);

        if (argc == 3 && (strcmp(argv[1], "path") == 0))
        {
                c_strcpy(settings.path, sizeof(settings.path), argv[2]);
                setConfig(&settings, ui);
                exit(0);
        }

        if (settings.path[0] == '\0')
        {
                setMusicPath();
        }

        atexit(cleanupOnExit);

        handleOptions(&argc, argv, ui);
        loadSpecialPlaylist(settings.path);

        if (argc == 1)
        {
                openLibrary(&appState);
        }
        else if (argc == 2 && strcmp(argv[1], "all") == 0)
        {
                playAll(&appState);
        }
        else if (argc == 2 && strcmp(argv[1], "albums") == 0)
        {
                playAllAlbums(&appState);
        }
        else if (argc == 2 && strcmp(argv[1], ".") == 0)
        {
                playSpecialPlaylist(&appState);
        }
        else if (argc >= 2)
        {
                init(&appState);
                makePlaylist(argc, argv, exactSearch, settings.path);
                if (playlist.count == 0)
                {
                        noPlaylist = true;
                        exit(0);
                }
                run(&appState);
        }

        return 0;
}
