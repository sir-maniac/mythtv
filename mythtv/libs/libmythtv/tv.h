#ifndef TV_H
#define TV_H

#include <qstring.h>
#include <pthread.h>

#include "NuppelVideoRecorder.h"
#include "NuppelVideoPlayer.h"
#include "RingBuffer.h"
#include "settings.h"

#include "channel.h"
#include "programinfo.h"

typedef enum 
{
    kState_Error = -1,
    kState_None = 0,
    kState_WatchingLiveTV,
    kState_WatchingPreRecorded,
    kState_WatchingRecording,       // watching _what_ you're recording
    kState_WatchingOtherRecording,  // watching something else
    kState_RecordingOnly
} TVState;

class QSqlDatabase;
class QDateTime;

class OSD;

class TV
{
 public:
    TV(const QString &startchannel);
   ~TV(void);

    TVState LiveTV(void);

    int AllowRecording(ProgramInfo *rcinfo, int timeuntil);
    void StartRecording(ProgramInfo *rcinfo);
    void StopRecording(void);
    ProgramInfo *GetRecording(void) { return curRecording; }

    // next two functions only work on recorded programs.
    void Playback(ProgramInfo *rcinfo);
    char *GetScreenGrab(ProgramInfo *rcinfo, int secondsin, int &bufferlen,
                        int &video_width, int &video_height);

    bool IsRunning(void) { return runMainLoop; }
    void Stop(void) { runMainLoop = false; }

    TVState GetState(void) { return internalState; }
    bool ChangingState(void) { return changeState; }
    
    bool CheckChannel(QString &channum); 
    bool ChangeExternalChannel(QString &channum);

    QString GetFilePrefix() { return settings->GetSetting("RecordFilePrefix"); }
    QString GetInstallPrefix();

 protected:
    void doLoadMenu(void);
    static void *MenuHandler(void *param);

    void RunTV(void);
    static void *EventThread(void *param);

 private:
    void ChangeChannel(bool up);
    void ChangeChannelByString(QString &name);
    
    void ChannelKey(int key);
    void ChannelCommit(void);

    void ToggleInputs(void); 

    void DoPause(void);
    void DoFF(void);
    void DoRew(void);
    int  calcSliderPos(int offset, QString &desc);
    
    void UpdateOSD(void); 
    void GetChannelInfo(QString lchannel, QString &title, QString &subtitle, 
                        QString &desc, QString &category, QString &starttime,
                        QString &endtime, QString &callsign, QString &iconpath);

    void ConnectDB(void);
    void DisconnectDB(void);

    void LoadMenu(void);

    void SetupRecorder();
    void TeardownRecorder(bool killFile = false);
    void SetupPipRecorder();
    void TeardownPipRecorder();
    
    void SetupPlayer();
    void TeardownPlayer();
    void SetupPipPlayer();
    void TeardownPipPlayer();
    
    void ProcessKeypress(int keypressed);

    void StateToString(TVState state, QString &statestr);
    void HandleStateChange();
    bool StateIsRecording(TVState state);
    bool StateIsPlaying(TVState state);
    TVState RemovePlaying(TVState state);
    TVState RemoveRecording(TVState state);

    void WriteRecordedRecord();

    void TogglePIPView(void);
    void ToggleActiveWindow(void);
    void SwapPIP(void);
    
    NuppelVideoRecorder *nvr;
    NuppelVideoPlayer *nvp;

    RingBuffer *rbuffer, *prbuffer;
    Channel *channel;

    Settings *settings;

    int osd_display_time;

    bool channelqueued;
    char channelKeys[4];
    int channelkeysstored;

    QSqlDatabase *db_conn;

    bool menurunning;

    TVState internalState;

    bool runMainLoop;
    bool exitPlayer;
    bool paused;
    QDateTime recordEndTime;

    float frameRate;

    pthread_t event, encode, decode, pipencode, pipdecode;
    bool changeState;
    TVState nextState;
    QString inputFilename, outputFilename;

    bool watchingLiveTV;

    ProgramInfo *curRecording;
    int tvtorecording;
    
    int playbackLen;

    int fftime;
    int rewtime;

    OSD *osd;

    NuppelVideoRecorder *pipnvr;
    NuppelVideoPlayer *pipnvp;
    Channel *pipchannel;
    RingBuffer *piprbuffer;

    NuppelVideoRecorder *activenvr;
    NuppelVideoPlayer *activenvp;
    RingBuffer *activerbuffer;
    Channel *activechannel;
};

#endif
