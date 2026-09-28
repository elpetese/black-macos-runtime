/**
 * macOS video pump implementation using AVFoundation.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <AVFoundation/AVFoundation.h>

#define VP_PATH_MAX 1024
static volatile int s_playing = 0;
static char s_path[VP_PATH_MAX];

static void *vp_thread_mac(void *arg)
{
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:@(s_path)];
        AVPlayer *player = [AVPlayer playerWithURL:url];
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];

        __block dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        id obs = [nc addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                                 object:player.currentItem
                                 queue:[NSOperationQueue mainQueue]
                             usingBlock:^(NSNotification *note) {
            dispatch_semaphore_signal(sem);
        }];

        [player play];
        dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(600 * NSEC_PER_SEC));
        if (dispatch_semaphore_wait(sem, deadline) != 0) {
            [player pause];
        }
        [nc removeObserver:obs];
        s_playing = 0;
    }
    return NULL;
}

int xbox_VideoPlayFile(const char *p)
{
    pthread_t th;
    if (!p || !*p)
        return -1;
    if (s_playing)
        return -1;
    strncpy(s_path, p, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';
    s_playing = 1;
    if (pthread_create(&th, NULL, vp_thread_mac, NULL) != 0) {
        s_playing = 0;
        return -1;
    }
    pthread_detach(th);
    return 0;
}

int xbox_VideoIsPlaying(void)
{
    return s_playing;
}
