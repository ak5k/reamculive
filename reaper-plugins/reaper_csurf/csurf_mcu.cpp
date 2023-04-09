/*
** reaper_csurf
** MCU support
** Copyright (C) 2006-2008 Cockos Incorporated
** License: LGPL.
*/
#include "reascript_vararg.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

#include "reaper_plugin_functions.h"

#include "csurf.h"

#define timeGetTime() GetTickCount64()

#define SPLASH_MESSAGE "ak5k MCU Live"

#undef BUFSIZ
#define BUFSIZ 256

namespace ReaMCULive {

static MediaTrack* GetOutputTrack()
{
    static MediaTrack* res {nullptr};
    if (res != nullptr && ValidatePtr2(0, res, "MediaTrack*")) {
        return res;
    }
    else {
        res = nullptr;
    }

    char buf[BUFSIZ] {};
    char gbuf[BUFSIZ] {};
    GUID* g {};
    GetProjExtState(0, "ak5k", "mculiveout", buf, BUFSIZ);

    for (int i = 0; i < GetNumTracks(); i++) {
        auto tr = GetTrack(0, i);
        g = GetTrackGUID(tr);
        guidToString(g, gbuf);
        if (strcmp(buf, gbuf) == 0) {
            res = tr;
        }
    }

    if (res == nullptr) {
        for (int i = 0; i < GetNumTracks(); i++) {
            auto tr = GetTrack(0, i);
            GetTrackName(tr, buf, BUFSIZ);
            for (size_t i = 0; buf[i] != '\0'; i++) {
                buf[i] = tolower(buf[i]);
            }
            if (strstr(buf, "mcu") && strstr(buf, "live")) {
                res = tr;
                break;
            }
        }
    }

    if (res != nullptr) {
        g = GetTrackGUID(res);
        guidToString(g, buf);
        SetProjExtState(0, "ak5k", "mculiveout", buf);
        return res;
    }

    return GetMasterTrack(0);
}

static MediaTrack* GetTrackFromID(int idx, bool mcpView)
{
    auto res = CSurf_TrackFromID(idx, mcpView);
    if (res != GetMasterTrack(0)) {
        return res;
    }
    return GetOutputTrack();
}
#define CSurf_TrackFromID GetTrackFromID

static double int14ToVol(unsigned char msb, unsigned char lsb)
{
    int val = lsb | (msb << 7);
    double pos = ((double)val * 1000.0) / 16383.0;
    pos = SLIDER2DB(pos);
    return DB2VAL(pos);
}
static double int14ToPan(unsigned char msb, unsigned char lsb)
{
    int val = lsb | (msb << 7);
    return 1.0 - (val / (16383.0 * 0.5));
}

static int volToInt14(double vol)
{
    double d = (DB2SLIDER(VAL2DB(vol)) * 16383.0 / 1000.0);
    if (d < 0.0)
        d = 0.0;
    else if (d > 16383.0)
        d = 16383.0;

    return (int)(d + 0.5);
}
static int panToInt14(double pan)
{
    double d = ((1.0 - pan) * 16383.0 * 0.5);
    if (d < 0.0)
        d = 0.0;
    else if (d > 16383.0)
        d = 16383.0;

    return (int)(d + 0.5);
}
static unsigned char volToChar(double vol)
{
    double d = (DB2SLIDER(VAL2DB(vol)) * 127.0 / 1000.0);
    if (d < 0.0)
        d = 0.0;
    else if (d > 127.0)
        d = 127.0;

    return (unsigned char)(d + 0.5);
}

static unsigned char panToChar(double pan)
{
    pan = (pan + 1.0) * 63.5;

    if (pan < 0.0)
        pan = 0.0;
    else if (pan > 127.0)
        pan = 127.0;

    return (unsigned char)(pan + 0.5);
}

class CSurf_MCULive;
static std::vector<CSurf_MCULive*> g_mcu_list;
static bool g_csurf_mcpmode {true}; // REAPER MCP / TCP

static int g_allmcus_bank_offset {};
static int g_split_bank_offset {};

static int g_flip_is_global {};
static int g_mode_is_global {-1 + (1 << 1)}; // mask for global modes

static int g_is_split {0};
static int g_split_offset {0};
static int g_split_point_idx {1}; // surface split point device index

std::mutex g_mutex;

typedef void (CSurf_MCULive::*ScheduleFunc)();

struct ScheduledAction {
    ScheduledAction(DWORD time, ScheduleFunc func)
    {
        this->next = NULL;
        this->time = time;
        this->func = func;
    }

    ScheduledAction* next;
    DWORD time;
    ScheduleFunc func;
};

#define CONFIG_FLAG_FADER_TOUCH_MODE 1

#define DOUBLE_CLICK_INTERVAL 250 /* ms */
MediaTrack* TrackFromGUID(const GUID& guid)
{
    // for (TrackIterator ti; !ti.end(); ++ti) {
    for (auto i = 0; i < GetNumTracks(); i++) {

        auto tr = GetTrack(0, i);
        const GUID* tguid = GetTrackGUID(tr);

        if (tr && tguid && !memcmp(tguid, &guid, sizeof(GUID)))
            return tr;
    }
    return NULL;
}

class CSurf_MCULive : public IReaperControlSurface {
  public:
    bool m_is_mcuex;
    int m_midi_in_dev;
    int m_midi_out_dev;
    int m_offset;
    int m_offset_orig;
    int m_size;
    int m_flipmode {};
    midi_Output* m_midiout;
    midi_Input* m_midiin;

    int m_vol_lastpos[8];
    int m_pan_lastpos[8];
    char m_mackie_lasttime[10];
    int m_mackie_lasttime_mode;
    int m_mackie_modifiers;
    int m_cfg_flags;      // CONFIG_FLAG_FADER_TOUCH_MODE etc
    int m_last_miscstate; // &1=metronome

    int m_button_map[BUFSIZ] {};
    int m_passthru_buttons[BUFSIZ] {};
    int m_press_only_buttons[BUFSIZ] {};
    int m_button_pressed[BUFSIZ] {};
    int m_button_remap[BUFSIZ] {};

    int m_page {};
    int m_mode {};            // mode assignment
    int m_modemask {};        // mode assignment mask
    int m_flipflags {1 << 0}; // allow flipmode flags
    int m_is_split;

    char m_fader_touchstate[8];
    unsigned int m_fader_lasttouch[8]; // m_fader_touchstate changes will
                                       // clear this, moves otherwise set it.
                                       // if set to -1, then totally disabled
    unsigned int m_pan_lasttouch[8];

    WDL_String m_descspace;
    char m_configtmp[4 * BUFSIZ];

    double m_mcu_meterpos[8];
    DWORD m_mcu_timedisp_lastforce, m_mcu_meter_lastrun;
    int m_mackie_arrow_states;
    unsigned int m_buttonstate_lastrun;
    unsigned int m_frameupd_lastrun;
    ScheduledAction* m_schedule;
    // SelectedTrack* m_selected_tracks;

// If user accidentally hits fader, we want to wait for user
// to stop moving fader and then reset it to it's orginal position
#define FADER_REPOS_WAIT 250
    bool m_repos_faders;
    DWORD m_fader_lastmove;

    int m_button_last;
    DWORD m_button_last_time;

    void ScheduleAction(DWORD time, ScheduleFunc func)
    {
        ScheduledAction* action = new ScheduledAction(time, func);
        // does not handle wrapping timestamp
        if (m_schedule == NULL) {
            m_schedule = action;
        }
        else if (action->time < m_schedule->time) {
            action->next = m_schedule;
            m_schedule = action;
        }
        else {
            ScheduledAction* curr = m_schedule;
            while (curr->next != NULL && curr->next->time < action->time)
                curr = curr->next;
            action->next = curr->next;
            curr->next = action;
        }
    }

    int GetBankOffset() const
    {
        return m_offset + 1 +
               (this->m_is_split ? g_split_bank_offset : g_allmcus_bank_offset);
    }

    void MCUReset()
    {
        std::sort(g_mcu_list.begin(), g_mcu_list.end(), CompareMCULiveOffset);

        memset(m_mackie_lasttime, 0, sizeof(m_mackie_lasttime));
        memset(m_fader_touchstate, 0, sizeof(m_fader_touchstate));
        memset(m_fader_lasttouch, 0, sizeof(m_fader_lasttouch));
        memset(m_pan_lasttouch, 0, sizeof(m_pan_lasttouch));
        m_mackie_lasttime_mode = -1;
        m_mackie_modifiers = 0;
        m_last_miscstate = 0;
        m_buttonstate_lastrun = 0;
        m_mackie_arrow_states = 0;

        memset(m_vol_lastpos, 0xff, sizeof(m_vol_lastpos));
        memset(m_pan_lastpos, 0xff, sizeof(m_pan_lastpos));

        if (m_midiout) {
            if (!m_is_mcuex) {
                m_midiout->Send(0x90, 0x32, m_flipmode ? 1 : 0, -1);
                m_midiout->Send(0x90, 0x33, g_csurf_mcpmode ? 0x7f : 0, -1);

                m_midiout->Send(
                    0x90,
                    0x64,
                    (m_mackie_arrow_states & 64) ? 0x7f : 0,
                    -1);
                m_midiout->Send(
                    0x90,
                    0x65,
                    (m_mackie_arrow_states & 128) ? 0x7f : 0,
                    -1);

                m_midiout->Send(
                    0xB0,
                    0x40 + 11,
                    '0' + (((g_allmcus_bank_offset + 1) / 10) % 10),
                    -1);
                m_midiout->Send(
                    0xB0,
                    0x40 + 10,
                    '0' + ((g_allmcus_bank_offset + 1) % 10),
                    -1);
            }

            UpdateMackieDisplay(0, SPLASH_MESSAGE, 56 * 2);

            int x;
            for (x = 0; x < 8; x++) {
                struct {
                    MIDI_event_t evt;
                    char data[9];
                } evt;
                evt.evt.frame_offset = 0;
                evt.evt.size = 9;
                unsigned char* wr = evt.evt.midi_message;
                wr[0] = 0xF0;
                wr[1] = 0x00;
                wr[2] = 0x00;
                wr[3] = 0x66;
                wr[4] = m_is_mcuex ? 0x15 : 0x14;
                wr[5] = 0x20;
                wr[6] = 0x00 + x;
                wr[7] = 0x03;
                wr[8] = 0xF7;
                Sleep(5);
                m_midiout->SendMsg(&evt.evt, -1);
            }
            Sleep(5);
            for (x = 0; x < 8; x++) {
                m_midiout->Send(0xD0, (x << 4) | 0xF, 0, -1);
            }
        }
    }

    void UpdateMackieDisplay(int pos, const char* text, int pad)
    {
        struct {
            MIDI_event_t evt;
            char data[2 * BUFSIZ];
        } evt;
        evt.evt.frame_offset = 0;
        unsigned char* wr = evt.evt.midi_message;
        wr[0] = 0xF0;
        wr[1] = 0x00;
        wr[2] = 0x00;
        wr[3] = 0x66;
        wr[4] = m_is_mcuex ? 0x15 : 0x14;
        wr[5] = 0x12;
        wr[6] = (unsigned char)pos;
        evt.evt.size = 7;

        int l = (int)strlen(text);
        if (pad < l)
            l = pad;
        if (l > 200)
            l = 200;

        int cnt = 0;
        while (cnt < l) {
            wr[evt.evt.size++] = *text++;
            cnt++;
        }
        while (cnt++ < pad)
            wr[evt.evt.size++] = ' ';
        wr[evt.evt.size++] = 0xF7;
        Sleep(5);
        m_midiout->SendMsg(&evt.evt, -1);
    }

    typedef bool (CSurf_MCULive::*MidiHandlerFunc)(MIDI_event_t*);

    bool OnMCUReset(MIDI_event_t* evt)
    {
        unsigned char onResetMsg[] = {
            0xf0,
            0x00,
            0x00,
            0x66,
            0x14,
            0x01,
            0x58,
            0x59,
            0x5a,
        };
        onResetMsg[4] = m_is_mcuex ? 0x15 : 0x14;
        if (evt->midi_message[0] == 0xf0 &&
            evt->size >= sizeof(onResetMsg)) // &&
        //    !memcmp(evt->midi_message, onResetMsg, sizeof(onResetMsg)))
        {
            // on reset
            MCUReset();
            TrackList_UpdateAllExternalSurfaces();
            return true;
        }
        return false;
    }

    bool OnFaderMove(MIDI_event_t* evt)
    {
        if ((evt->midi_message[0] & 0xf0) == 0xe0) // volume fader move
        {
            m_fader_lastmove = timeGetTime();

            int tid = evt->midi_message[0] & 0xf;
            if (tid >= 0 && tid < 9 && m_fader_lasttouch[tid] != 0xffffffff)
                m_fader_lasttouch[tid] = m_fader_lastmove;

            if (tid == 8)
                tid = 0; // master offset, master=0
            else
                tid += GetBankOffset();

            MediaTrack* tr = CSurf_TrackFromID(tid, g_csurf_mcpmode);

            if (tr) {
                if ((m_cfg_flags & CONFIG_FLAG_FADER_TOUCH_MODE) &&
                    !GetTouchState(tr)) {
                    m_repos_faders = true;
                }

                double val;
                if (m_flipmode) {
                    val =
                        int14ToPan(evt->midi_message[2], evt->midi_message[1]);
                }
                else {
                    val =
                        int14ToVol(evt->midi_message[2], evt->midi_message[1]);
                }
                if (m_mode == 1) {
                    if (m_flipmode) {
                        val = CSurf_OnPanChange(tr, val, false);
                        CSurf_SetSurfacePan(tr, val, NULL);
                    }
                    else {
                        val = CSurf_OnVolumeChange(tr, val, false);
                        CSurf_SetSurfaceVolume(tr, val, NULL);
                    }
                }
                if (m_mode == 2) {
                    MediaTrack* dst {nullptr};
                    for (int i = 0; i < GetTrackNumSends(tr, 0); i++) {
                        dst = (MediaTrack*)(uintptr_t)
                            GetTrackSendInfo_Value(tr, 0, i, "P_DESTTRACK");
                        if (GetSelectedTrack(0, 0) == dst) {
                            if (m_flipmode) {
                                (void)CSurf_OnSendPanChange(tr, i, val, false);
                            }
                            else {
                                (void)
                                    CSurf_OnSendVolumeChange(tr, i, val, false);
                            }
                            break;
                        }
                    }
                    if (GetSelectedTrack(0, 0) != dst) {
                        return true; // send not found
                    }
                }
            }
            return true;
        }
        return false;
    }

    bool OnRotaryEncoder(MIDI_event_t* evt)
    {
        if ((evt->midi_message[0] & 0xf0) == 0xb0 &&
            evt->midi_message[1] >= 0x10 && evt->midi_message[1] < 0x18) // pan
        {
            int tid = evt->midi_message[1] - 0x10;

            m_pan_lasttouch[tid & 7] = timeGetTime();

            if (tid == 8)
                tid = 0; // adjust for master
            else
                tid += GetBankOffset();
            MediaTrack* tr = CSurf_TrackFromID(tid, g_csurf_mcpmode);
            if (tr) {
                double adj = (evt->midi_message[2] & 0x3f) / 31.0;
                if (evt->midi_message[2] & 0x40)
                    adj = -adj;

                double val;
                if (m_mode == 1) {
                    if (m_flipmode) {
                        val = CSurf_OnVolumeChange(tr, adj * 11.0, true);
                        CSurf_SetSurfaceVolume(tr, val, NULL);
                    }
                    else {
                        val = CSurf_OnPanChange(tr, adj, true);
                        CSurf_SetSurfacePan(tr, val, NULL);
                    }
                }

                if (m_mode == 2) {
                    MediaTrack* dst {nullptr};
                    for (int i = 0; i < GetTrackNumSends(tr, 0); i++) {
                        dst = (MediaTrack*)(uintptr_t)
                            GetTrackSendInfo_Value(tr, 0, i, "P_DESTTRACK");
                        if (GetSelectedTrack(0, 0) == dst) {
                            if (m_flipmode) {
                                (void)CSurf_OnSendVolumeChange(
                                    tr,
                                    i,
                                    adj * 11.0,
                                    true);
                            }
                            else {
                                (void)CSurf_OnSendPanChange(tr, i, adj, true);
                            }
                            break;
                        }
                    }
                    if (GetSelectedTrack(0, 0) != dst) {
                        return true; // send not found
                    }
                }
            }
            return true;
        }
        return false;
    }

    bool OnJogWheel(MIDI_event_t* evt)
    {
        if ((evt->midi_message[0] & 0xf0) == 0xb0 &&
            evt->midi_message[1] == 0x3c) // jog wheel
        {
            if (evt->midi_message[2] >= 0x41)
                CSurf_OnRewFwd(
                    m_mackie_arrow_states & 128,
                    0x40 - (int)evt->midi_message[2]);
            else if (evt->midi_message[2] > 0 && evt->midi_message[2] < 0x40)
                CSurf_OnRewFwd(
                    m_mackie_arrow_states & 128,
                    evt->midi_message[2]);
            return true;
        }
        return false;
    }

    bool OnRotaryEncoderPush(MIDI_event_t* evt)
    {
        int trackid = evt->midi_message[1] - 0x20;
        m_pan_lasttouch[trackid] = timeGetTime();

        trackid += GetBankOffset();

        MediaTrack* tr = CSurf_TrackFromID(trackid, g_csurf_mcpmode);
        if (tr) {
            if (m_flipmode) {
                CSurf_SetSurfaceVolume(
                    tr,
                    CSurf_OnVolumeChange(tr, 1.0, false),
                    NULL);
            }
            else {
                CSurf_SetSurfacePan(
                    tr,
                    CSurf_OnPanChange(tr, 0.0, false),
                    NULL);
            }
        }
        return true;
    }

    int GetSendIndex(MediaTrack* src, MediaTrack* dst = GetSelectedTrack(0, 0))
    {
        for (int i = 0; i < GetTrackNumSends(src, 0); i++) {
            auto tr = (MediaTrack*)(uintptr_t)
                GetTrackSendInfo_Value(src, 0, i, "P_DESTTRACK");
            if (tr == dst) {
                return i;
            }
        }
        return -1;
    }

    bool isSendMuted(MediaTrack* tr, int idx)
    {
        if (idx >= 0) {
            return (bool)GetTrackSendInfo_Value(tr, 0, idx, "B_MUTE");
        }
        else {
            return true;
        }
    }

    bool OnMuteSolo(MIDI_event_t* evt)
    {
        int tid = evt->midi_message[1] - 0x08;
        int ismute = (tid & 8);
        tid &= 7;
        tid += GetBankOffset();

        MediaTrack* tr = CSurf_TrackFromID(tid, g_csurf_mcpmode);
        if (tr) {
            if (ismute)
                if (m_mode == 2) {
                    auto idx = GetSendIndex(tr);
                    if (idx < 0) {
                        idx = CreateTrackSend(tr, GetSelectedTrack(0, 0));
                        SetTrackSendInfo_Value(tr, 0, idx, "B_MUTE", 1);
                    }
                    auto isMuted = isSendMuted(tr, idx);
                    SetTrackSendInfo_Value(tr, 0, idx, "B_MUTE", !isMuted);
                    SetSurfaceMute(tr, !isMuted);
                }
                else {
                    CSurf_SetSurfaceMute(tr, CSurf_OnMuteChange(tr, -1), NULL);
                }
            else
                CSurf_SetSurfaceSolo(tr, CSurf_OnSoloChange(tr, -1), NULL);
        }
        return true;
    }

    bool OnSoloDC(MIDI_event_t* evt)
    {
        int tid = evt->midi_message[1] - 0x08;
        tid += GetBankOffset();
        MediaTrack* tr = CSurf_TrackFromID(tid, g_csurf_mcpmode);
        SoloAllTracks(0);
        CSurf_SetSurfaceSolo(tr, CSurf_OnSoloChange(tr, 1), NULL);
        return true;
    }

    bool OnChannelSelect(MIDI_event_t* evt)
    {
        int tid = evt->midi_message[1] - 0x18;
        tid &= 7;
        tid += GetBankOffset();
        MediaTrack* tr = CSurf_TrackFromID(tid, g_csurf_mcpmode);
        if (tr) {
            CSurf_OnSelectedChange(
                tr,
                -1); // this will automatically update the surface
        }
        if (g_is_split) {
            TrackList_UpdateAllExternalSurfaces();
        }
        return true;
    }

    bool OnChannelSelectDC(MIDI_event_t* evt)
    {
        int tid = evt->midi_message[1] - 0x18;
        tid &= 7;
        tid += GetBankOffset();
        MediaTrack* tr = CSurf_TrackFromID(tid, g_csurf_mcpmode);
        SetOnlyTrackSelected(tr);
        CSurf_OnSelectedChange(tr, 1);
        if (g_is_split) {
            TrackList_UpdateAllExternalSurfaces();
        }

        return true;
    }
    int GetFirstSetBitPosition(int n)
    {
        if (!n) {
            return n;
        }
        int i = 1, pos = 1;

        while (!(i & n)) {
            i = i << 1;
            ++pos;
        }
        return pos;
    }

    bool OnModeSet(MIDI_event_t* evt)
    {
        auto mode = evt->midi_message[1] - 0x28;
        auto modemask = m_modemask;

        if (modemask & 1 << mode) {
            modemask ^= 1 << mode;
        }
        else {
            if (g_mode_is_global & 1 << mode) {
                modemask &= ~g_mode_is_global;
            }
            else {
                modemask &= g_mode_is_global;
            }
            modemask |= 1 << mode;
        }

        mode++; // modes 0 ... 5 to  1 ... 6

        if (!modemask) {
            modemask = 1 << 0;
            m_mode = modemask;
        }

        if (!(modemask & g_mode_is_global)) {
            modemask |= 1;
        }

        int n {0};
        for (auto mcu : g_mcu_list) {
            if (mcu) {
                for (int i = 0; i < 6; i++) {
                    if (mcu->m_modemask & 1 << i && !(modemask & 1 << i)) {
                        if (mcu->m_midiout)
                            mcu->m_midiout->Send(0x90, 0x28 + i, 0, -1);
                    }
                    if (modemask & 1 << i) {
                        if (mcu->m_midiout)
                            mcu->m_midiout->Send(0x90, 0x28 + i, 1, -1);
                    }
                }

                mcu->m_modemask = modemask;

                mcu->m_mode =
                    GetFirstSetBitPosition(mcu->m_modemask & g_mode_is_global);

                if (n == g_split_point_idx) {
                    g_split_offset = mcu->m_offset_orig;
                }
                if (!(n < g_split_point_idx)) {
                    if (modemask & ~g_mode_is_global) {
                        // here console split
                        g_is_split = 1;
                        mcu->m_is_split = 1;
                        mcu->m_offset = mcu->m_offset_orig - g_split_offset;
                        mcu->m_mode = GetFirstSetBitPosition(
                            modemask & ~g_mode_is_global);
                    }
                    else {
                        g_is_split = 0;
                        mcu->m_is_split = 0;
                        mcu->m_offset = mcu->m_offset_orig;
                    }
                }
                n++;

                if (mcu->m_flipmode && !(m_flipflags & 1 << mcu->m_mode)) {
                    mcu->OnFlip(evt);
                }
            }
        }

        CSurf_ResetAllCachedVolPanStates();
        TrackList_UpdateAllExternalSurfaces();

        return true;
    }

    bool OnFlip(MIDI_event_t* evt)
    {
        if ((m_flipmode && !(m_flipflags & m_modemask)) ||
            m_flipflags & m_modemask) {
            ;
        }
        else {
            return false;
        };
        m_flipmode = ~m_flipmode;
        if (m_midiout)
            m_midiout->Send(0x90, 0x32, m_flipmode ? 1 : 0, -1);
        CSurf_ResetAllCachedVolPanStates();
        TrackList_UpdateAllExternalSurfaces();
        return true;
    }

    bool OnTouch(MIDI_event_t* evt)
    {
        int fader = evt->midi_message[1] - 0x68;
        m_fader_touchstate[fader] = evt->midi_message[2] >= 0x7f;
        m_fader_lasttouch[fader] = 0xFFFFFFFF; // never use this again!
        return true;
    }
    bool OnMCULiveButton(MIDI_event_t* evt)
    {
        auto msg = evt->midi_message[1];
        int command {};
        {
            // std::scoped_lock lock {g_mutex};
            command = m_button_map[msg];
        }
        if (!command) {
            switch (msg) {
            case 0x28:
            case 0x29:
            case 0x2a:
            case 0x2b:
            case 0x2c:
            case 0x2d:
                OnModeSet(evt);
                break;
            case 0x5e:
                // play : tap tempo
                Main_OnCommand(1134, 0);
                break;
            case 0x32:
                OnFlip(evt);
                break;
            }
        }
        else {
            Main_OnCommand(command, 0);
        }

        return true;
    }

    struct ButtonHandler {
        unsigned int evt_min;
        unsigned int evt_max; // inclusive
        MidiHandlerFunc func;
        MidiHandlerFunc func_dc;
    };

    bool OnButtonPress(MIDI_event_t* evt)
    {
        if ((evt->midi_message[0] & 0xf0) != 0x90)
            return false;

        if (m_button_remap[evt->midi_message[1]]) {
            evt->midi_message[1] = m_button_remap[evt->midi_message[1]];
        }

        m_button_pressed[evt->midi_message[1]] = evt->midi_message[2];

        static const int nHandlers = 8;
        static const int nPressOnlyHandlers = 4;
        static const ButtonHandler handlers[nHandlers] = {
            //
            {0x00, 0x07, &CSurf_MCULive::OnRecArm, NULL},
            {0x08, 0x0f, NULL, &CSurf_MCULive::OnSoloDC},
            {0x08, 0x17, &CSurf_MCULive::OnMuteSolo, NULL},
            {0x18,
             0x1f,
             &CSurf_MCULive::OnChannelSelectDC,
             &CSurf_MCULive::OnChannelSelect},
            // Press and release events
            {0x28, 0x45, &CSurf_MCULive::OnMCULiveButton, NULL},
            {0x4a, 0x5f, &CSurf_MCULive::OnMCULiveButton, NULL},
            {0x64, 0x67, &CSurf_MCULive::OnMCULiveButton, NULL},
            {0x68, 0x70, &CSurf_MCULive::OnTouch},
        };

        unsigned int evt_code =
            evt->midi_message[1]; // get_midi_evt_code( evt );

#if 0
      char buf[512];
      sprintf( buf, "   0x%08x %02x %02x %02x %02x 0x%08x 0x%08x %s", evt_code,
          evt->midi_message[0], evt->midi_message[1], evt->midi_message[2], evt->midi_message[3],
          handlers[0].evt_min, handlers[0].evt_max, 
          handlers[0].evt_min <= evt_code && evt_code <= handlers[0].evt_max ? "yes" : "no" );
      UpdateMackieDisplay( 0, buf, 56 );
#endif

        // For these events we only want to track button press
        if (m_press_only_buttons[evt_code] && evt->midi_message[2] >= 0x40) {
            // Check for double click
            DWORD now = timeGetTime();
            bool double_click =
                (int)evt_code == m_button_last &&
                now - m_button_last_time < DOUBLE_CLICK_INTERVAL;
            m_button_last = evt_code;
            m_button_last_time = now;

            // Find event handler
            // for (int i = 0; i < nPressOnlyHandlers; i++) {
            for (int i = 0; i < nHandlers; i++) {
                ButtonHandler bh = handlers[i];
                if (bh.evt_min <= evt_code && evt_code <= bh.evt_max) {
                    // Try double click first
                    if (double_click && bh.func_dc != NULL)
                        if ((this->*bh.func_dc)(evt))
                            return true;

                    // Single click (and unhandled double clicks)
                    if (bh.func != NULL)
                        if ((this->*bh.func)(evt))
                            return true;
                }
            }
        }

        // For these events we want press and release
        for (int i = nPressOnlyHandlers; i < nHandlers; i++)
            if (!m_press_only_buttons[evt_code] &&
                handlers[i].evt_min <= evt_code &&
                evt_code <= handlers[i].evt_max)
                if ((this->*handlers[i].func)(evt))
                    return true;

        // Pass thru if not otherwise handled
        if (evt->midi_message[2] >= 0x40) {
            int a = evt->midi_message[1];
            MIDI_event_t evt = {
                0,
                3,
                {(unsigned char)(0xbf - (m_mackie_modifiers & 15)),
                 (unsigned char)a,
                 0}};
            kbd_OnMidiEvent(&evt, -1);
        }

        return true;
    }

    void OnMIDIEvent(MIDI_event_t* evt)
    {
#if 0
        char buf[512];
        sprintf(
            buf,
            "message %02x, %02x, %02x\n",
            evt->midi_message[0],
            evt->midi_message[1],
            evt->midi_message[2]);
        OutputDebugString(buf);
#endif

        static const int nHandlers = 5;
        static const MidiHandlerFunc handlers[nHandlers] = {
            &CSurf_MCULive::OnMCUReset,
            &CSurf_MCULive::OnFaderMove,
            &CSurf_MCULive::OnRotaryEncoder,
            &CSurf_MCULive::OnJogWheel,
            &CSurf_MCULive::OnButtonPress,
        };
        for (int i = 0; i < nHandlers; i++)
            if ((this->*handlers[i])(evt))
                return;
    }
    static bool CompareMCULiveOffset(
        const ReaMCULive::CSurf_MCULive* a,
        const ReaMCULive::CSurf_MCULive* b)
    {
        return a->m_offset < b->m_offset;
    }
    CSurf_MCULive(
        bool ismcuex,
        int offset,
        int size,
        int indev,
        int outdev,
        int cfgflags,
        int* errStats)
    {
        m_cfg_flags = cfgflags;

        m_is_mcuex = ismcuex;
        m_offset = offset;
        m_offset_orig = m_offset;
        m_size = size;
        m_midi_in_dev = indev;
        m_midi_out_dev = outdev;

        m_mode = 1;
        m_modemask = 1;
        m_is_split = 0;

        g_mcu_list.push_back(this);

        // init locals
        for (int x = 0; x < sizeof(m_mcu_meterpos) / sizeof(m_mcu_meterpos[0]);
             x++)
            m_mcu_meterpos[x] = -100000.0;
        m_mcu_timedisp_lastforce = 0;
        m_mcu_meter_lastrun = 0;
        memset(m_fader_touchstate, 0, sizeof(m_fader_touchstate));
        memset(m_fader_lasttouch, 0, sizeof(m_fader_lasttouch));
        memset(m_pan_lasttouch, 0, sizeof(m_pan_lasttouch));

        memset(m_button_map, 0, sizeof(m_button_map));
        memset(m_button_remap, 0, sizeof(m_button_remap));
        memset(m_passthru_buttons, 0, sizeof(m_passthru_buttons));
        memset(m_press_only_buttons, 1, sizeof(m_press_only_buttons));
        memset(m_button_pressed, 0, sizeof(m_button_pressed));

        // m_button_remap[0x32] = 0x29; // flip to sends

        // create midi hardware access
        m_midiin = m_midi_in_dev >= 0 ? CreateMIDIInput(m_midi_in_dev) : NULL;
        m_midiout = m_midi_out_dev >= 0
                        ? CreateThreadedMIDIOutput(
                              CreateMIDIOutput(m_midi_out_dev, false, NULL))
                        : NULL;

        if (errStats) {
            if (m_midi_in_dev >= 0 && !m_midiin)
                *errStats |= 1;
            if (m_midi_out_dev >= 0 && !m_midiout)
                *errStats |= 2;
        }

        MCUReset();

        if (m_midiin)
            m_midiin->start();

        m_repos_faders = false;
        m_schedule = NULL;
    }

    ~CSurf_MCULive()
    {
        g_mcu_list.erase(
            std::remove(g_mcu_list.begin(), g_mcu_list.end(), this),
            g_mcu_list.end());

        if (m_midiout) {

#if 1 // reset MCU to stock!, fucko enable this in dist builds, maybe?
            struct {
                MIDI_event_t evt;
                char data[5];
            } evt;
            evt.evt.frame_offset = 0;
            evt.evt.size = 8;
            unsigned char* wr = evt.evt.midi_message;
            wr[0] = 0xF0;
            wr[1] = 0x00;
            wr[2] = 0x00;
            wr[3] = 0x66;
            wr[4] = m_is_mcuex ? 0x15 : 0x14;
            wr[5] = 0x08;
            wr[6] = 0x00;
            wr[7] = 0xF7;
            Sleep(5);
            m_midiout->SendMsg(&evt.evt, -1);
            Sleep(5);

#elif 0
            char bla[11] = {"          "};
            int x;
            for (x = 0; x < sizeof(bla) - 1; x++)
                m_midiout->Send(0xB0, 0x40 + x, bla[x], -1);
            UpdateMackieDisplay(0, "", 56 * 2);
#endif
        }
        DELETE_ASYNC(m_midiout);
        DELETE_ASYNC(m_midiin);
        while (m_schedule != NULL) {
            ScheduledAction* temp = m_schedule;
            m_schedule = temp->next;
            delete temp;
        }
    }

    const char* GetTypeString()
    {
        return m_is_mcuex ? "MCULIVEEX" : "MCULIVE";
    }
    const char* GetDescString()
    {
        m_descspace.SetFormatted(
            512,
            m_is_mcuex
                ? __LOCALIZE_VERFMT("MCU Live Extender (dev %d,%d)", "csurf")
                : __LOCALIZE_VERFMT("MCU Live (dev %d,%d)", "csurf"),
            m_midi_in_dev,
            m_midi_out_dev);
        return m_descspace.Get();
    }
    const char* GetConfigString() // string of configuration data
    {
        snprintf(
            m_configtmp,
            sizeof(m_configtmp),
            "%d %d %d %d %d",
            m_offset,
            m_size,
            m_midi_in_dev,
            m_midi_out_dev,
            m_cfg_flags);
        return m_configtmp;
    }

    void CloseNoReset()
    {
        DELETE_ASYNC(m_midiout);
        DELETE_ASYNC(m_midiin);
        m_midiout = 0;
        m_midiin = 0;
    }

    void RunOutput(DWORD now);

    void Run()
    {
        DWORD now = timeGetTime();

        if ((int)(now - m_frameupd_lastrun) >=
            (1000 / std::max((*g_config_csurf_rate), 1))) {
            m_frameupd_lastrun = now;

            while (m_schedule && (now - m_schedule->time) < 0x10000000) {
                ScheduledAction* action = m_schedule;
                m_schedule = m_schedule->next;
                (this->*(action->func))();
                delete action;
            }

            RunOutput(now);
        }

        if (m_midiin) {
            m_midiin->SwapBufs(timeGetTime());
            int l = 0;
            MIDI_eventlist* list = m_midiin->GetReadBuf();
            MIDI_event_t* evts;
            while ((evts = list->EnumItems(&l)))
                OnMIDIEvent(evts);

            if (m_mackie_arrow_states) {
                DWORD now = timeGetTime();
                if ((now - m_buttonstate_lastrun) >= 100) {
                    m_buttonstate_lastrun = now;

                    if (m_mackie_arrow_states) {
                        int iszoom = m_mackie_arrow_states & 64;

                        if (m_mackie_arrow_states & 1)
                            CSurf_OnArrow(0, !!iszoom);
                        if (m_mackie_arrow_states & 2)
                            CSurf_OnArrow(1, !!iszoom);
                        if (m_mackie_arrow_states & 4)
                            CSurf_OnArrow(2, !!iszoom);
                        if (m_mackie_arrow_states & 8)
                            CSurf_OnArrow(3, !!iszoom);
                    }
                }
            }
        }

        if (m_repos_faders && now >= m_fader_lastmove + FADER_REPOS_WAIT) {
            m_repos_faders = false;
            TrackList_UpdateAllExternalSurfaces();
        }
    }

    void SetTrackListChange()
    {
        if (m_midiout) {
            int x;
            for (x = 0; x < 8; x++) {
                MediaTrack* t =
                    CSurf_TrackFromID(x + GetBankOffset(), g_csurf_mcpmode);
                if (!t || t == CSurf_TrackFromID(0, false)) {
                    // clear item
                    int panint = m_flipmode ? panToInt14(0.0) : volToInt14(0.0);
                    unsigned char volch =
                        m_flipmode ? volToChar(0.0) : panToChar(0.0);

                    m_midiout->Send(
                        0xe0 + (x & 0xf),
                        panint & 0x7f,
                        (panint >> 7) & 0x7f,
                        -1);
                    m_midiout->Send(
                        0xb0,
                        0x30 + (x & 0xf),
                        1 + ((volch * 11) >> 7),
                        -1);
                    m_vol_lastpos[x] = panint;

                    m_midiout->Send(0x90, 0x10 + (x & 7), 0, -1); // reset mute
                    m_midiout->Send(
                        0x90,
                        0x18 + (x & 7),
                        0,
                        -1); // reset selected

                    m_midiout->Send(0x90, 0x08 + (x & 7), 0, -1); // reset solo
                    m_midiout->Send(0x90, 0x0 + (x & 7), 0, -1); // reset recarm

                    char buf[7] = {
                        0,
                    };
                    UpdateMackieDisplay(x * 7, buf, 7); // clear display

                    struct {
                        MIDI_event_t evt;
                        char data[9];
                    } evt;
                    evt.evt.frame_offset = 0;
                    evt.evt.size = 9;
                    unsigned char* wr = evt.evt.midi_message;
                    wr[0] = 0xF0;
                    wr[1] = 0x00;
                    wr[2] = 0x00;
                    wr[3] = 0x66;
                    wr[4] = m_is_mcuex ? 0x15 : 0x14;
                    wr[5] = 0x20;
                    wr[6] = 0x00 + x;
                    wr[7] = 0x03;
                    wr[8] = 0xF7;
                    Sleep(5);
                    m_midiout->SendMsg(&evt.evt, -1);
                    Sleep(5);
                    m_midiout->Send(0xD0, (x << 4) | 0xF, 0, -1);
                }
            }
        }
    }
#define FIXID(id)                                              \
    const int oid = CSurf_TrackToID(trackid, g_csurf_mcpmode); \
    int id = oid;                                              \
    if (id > 0) {                                              \
        id -= GetBankOffset();                                 \
        if (id == 8)                                           \
            id = -1;                                           \
    }                                                          \
    else if (id == 0)                                          \
        id = 8;

    void SetSurfaceVolume(MediaTrack* trackid, double volume)
    {
        auto hasMcuMaster {false};
        auto mcuMaster = GetOutputTrack();
        if (mcuMaster != GetMasterTrack(0)) {
            hasMcuMaster = true;
        }

        FIXID(id)

        // ignore standard master
        if (hasMcuMaster && id == 8) {
            id = -1;
        }

        if (this->m_mode == 2) {
            volume = GetSendLevel(trackid);
        }

        if (m_midiout && id >= 0 && id < 256 && id < m_size) {
            if (m_flipmode) {
                unsigned char volch = volToChar(volume);
                if (id < 8)
                    m_midiout->Send(
                        0xb0,
                        0x30 + (id & 0xf),
                        1 + ((volch * 11) >> 7),
                        -1);
            }
            else {
                int volint = volToInt14(volume);

                if (m_vol_lastpos[id] != volint) {
                    m_vol_lastpos[id] = volint;
                    m_midiout->Send(
                        0xe0 + (id & 0xf),
                        volint & 0x7f,
                        (volint >> 7) & 0x7f,
                        -1);
                }
            }
        }

        // discrete master
        if (m_midiout && hasMcuMaster && trackid == mcuMaster && !m_flipmode) {
            id = 8;
            int volint = volToInt14(volume);
            if (m_vol_lastpos[id] != volint) {
                m_vol_lastpos[id] = volint;
                m_midiout->Send(
                    0xe0 + (id & 0xf),
                    volint & 0x7f,
                    (volint >> 7) & 0x7f,
                    -1);
            }
        }
    }

    double GetSendLevel(
        MediaTrack* src,
        MediaTrack* dst = GetSelectedTrack(0, 0))
    {
        double res {0};
        if (!dst) {
            return res;
        }
        for (int i = 0; i < GetTrackNumSends(src, 0); i++) {
            auto tr = (MediaTrack*)(uintptr_t)
                GetTrackSendInfo_Value(src, 0, i, "P_DESTTRACK");
            if (tr == dst) {
                res = GetTrackSendInfo_Value(src, 0, i, "D_VOL");
                break;
            }
        }
        return res;
    }

    void SetSurfacePan(MediaTrack* trackid, double pan)
    {
        if (this->m_mode == 2) {
            auto idx = GetSendIndex(trackid);
            pan = GetTrackSendInfo_Value(trackid, 0, idx, "D_PAN");
        }
        FIXID(id)
        if (m_midiout && id >= 0 && id < 256 && id < m_size) {
            unsigned char panch = panToChar(pan);
            if (m_pan_lastpos[id] != panch) {
                m_pan_lastpos[id] = panch;

                if (m_flipmode) {
                    int panint = panToInt14(pan);
                    m_vol_lastpos[id] = panint;
                    m_midiout->Send(
                        0xe0 + (id & 0xf),
                        panint & 0x7f,
                        (panint >> 7) & 0x7f,
                        -1);
                }
                else {
                    if (id < 8)
                        m_midiout->Send(
                            0xb0,
                            0x30 + (id & 0xf),
                            1 + ((panch * 11) >> 7),
                            -1);
                }
            }
        }
    }
    void SetSurfaceMute(MediaTrack* trackid, bool mute)
    {
        if (m_mode == 2) {
            mute = isSendMuted(trackid, GetSendIndex(trackid));
        }

        FIXID(id)

        if (m_midiout && id >= 0 && id < 256 && id < m_size) {
            if (id < 8) {
                m_midiout->Send(0x90, 0x10 + (id & 7), mute ? 0x7f : 0, -1);
            }
        }
    }

    void SetSurfaceSelected(MediaTrack* trackid, bool selected)
    {

        FIXID(id)
        if (m_midiout && id >= 0 && id < 256 && id < m_size) {
            if (id < 8)
                m_midiout->Send(0x90, 0x18 + (id & 7), selected ? 0x7f : 0, -1);
        }
    }

    void SetSurfaceSolo(MediaTrack* trackid, bool solo)
    {
        FIXID(id)
        if (m_midiout && id >= 0 && id < 256 && id < m_size) {
            if (id < 8)
                m_midiout->Send(
                    0x90,
                    0x08 + (id & 7),
                    solo ? 1 : 0,
                    -1); // blink
            else if (id == 8) {
                // Hmm, seems to call this with id 8 to tell if any
                // tracks are soloed.
                m_midiout
                    ->Send(0x90, 0x73, solo ? 1 : 0, -1); // rude solo light
                m_midiout->Send(
                    0x90,
                    0x5a,
                    solo ? 0x7f : 0,
                    -1); // solo button led
            }
        }
    }

    void SetSurfaceRecArm(MediaTrack* trackid, bool recarm)
    {
        (void)trackid;
        (void)recarm;
        return;
        // FIXID(id)
        // if (m_midiout && id >= 0 && id < 256 && id < m_size) {
        //     if (id < 8) {
        //         m_midiout->Send(0x90, 0x0 + (id & 7), recarm ? 0x7f : 0,
        //         -1);
        //     }
        // }
    }

    void SetPlayState(bool play, bool pause, bool rec)
    {
        if (m_midiout && !m_is_mcuex) {
            m_midiout->Send(0x90, 0x5f, rec ? 0x7f : 0, -1);
            m_midiout->Send(0x90, 0x5e, play || pause ? 0x7f : 0, -1);
            m_midiout->Send(0x90, 0x5d, !play ? 0x7f : 0, -1);
        }
    }

    void SetTrackTitle(MediaTrack* trackid, const char* title)
    {
        FIXID(id)
        if (m_midiout && id >= 0 && id < 8) {
            char buf[32];
            strncpy(buf, title, 6);
            buf[6] = 0;
            if (strlen(buf) == 0) {
                int trackno = CSurf_TrackToID(trackid, g_csurf_mcpmode);
                if (trackno < 100)
                    snprintf(buf, sizeof(buf), "  %02d  ", trackno);
                else
                    snprintf(buf, sizeof(buf), "  %d ", trackno);
            }
            UpdateMackieDisplay(id * 7, buf, 7);
        }
    }
    bool GetTouchState(MediaTrack* trackid, int isPan = 0)
    {
        if (isPan != 0 && isPan != 1)
            return false;

        FIXID(id)
        if (~m_flipmode != ~isPan) {
            if (id >= 0 && id < 8) {
                if (m_pan_lasttouch[id] == 1 ||
                    (timeGetTime() - m_pan_lasttouch[id]) <
                        3000) // fake touch, go for 3s after last movement
                {
                    return true;
                }
            }
            return false;
        }
        if (id >= 0 && id < 9) {
            if (!(m_cfg_flags & CONFIG_FLAG_FADER_TOUCH_MODE) &&
                !m_fader_touchstate[id] && m_fader_lasttouch[id] &&
                m_fader_lasttouch[id] != 0xffffffff) {
                if ((timeGetTime() - m_fader_lasttouch[id]) < 3000)
                    return true;
                return false;
            }

            return !!m_fader_touchstate[id];
        }

        return false;
    }

    void ResetCachedVolPanStates()
    {
        memset(m_vol_lastpos, 0xff, sizeof(m_vol_lastpos));
        memset(m_pan_lastpos, 0xff, sizeof(m_pan_lastpos));
    }

    void OnTrackSelection(MediaTrack* trackid)
    {
        int tid = CSurf_TrackToID(trackid, g_csurf_mcpmode);
        // if no normal MCU's here, then slave it
        int movesize = 8;
        int n {0};
        for (auto mcu : g_mcu_list) {
            if (mcu) {
                if (g_is_split && n == g_split_point_idx) {
                    break;
                }
                if (mcu->m_offset + 8 > movesize)
                    movesize = mcu->m_offset + 8;
            }
            n++;
        }
        int newpos = tid - 1;
        if (newpos >= 0 && (newpos < g_allmcus_bank_offset ||
                            newpos >= g_allmcus_bank_offset + movesize)) {
            int no = newpos - (newpos % movesize);

            if (no != g_allmcus_bank_offset) {
                g_allmcus_bank_offset = no;
                // update all of the sliders

                TrackList_UpdateAllExternalSurfaces();
                n = 0;
                for (auto mcu : g_mcu_list) {
                    if (g_is_split && n == g_split_point_idx) {
                        break;
                    }
                    n++;
                    if (mcu && mcu->m_midiout) {
                        mcu->m_midiout->Send(
                            0x90,
                            0x0 + (mcu->m_page & 7),
                            0,
                            -1); // 0x7f : 0
                        if (mcu == this) {
                            mcu->m_page = (tid - 1) / movesize;
                            mcu->m_midiout->Send(
                                0x90,
                                0x0 + (mcu->m_page & 7),
                                0x7f,
                                -1); // 0x7f : 0
                        }
                    }
                    if (mcu && !mcu->m_is_mcuex && mcu->m_midiout) {
                        mcu->m_midiout->Send(
                            0xB0,
                            0x40 + 11,
                            '0' + (((g_allmcus_bank_offset + 1) / 10) % 10),
                            -1);
                        mcu->m_midiout->Send(
                            0xB0,
                            0x40 + 10,
                            '0' + ((g_allmcus_bank_offset + 1) % 10),
                            -1);
                    }
                }
            }
        }
        else if (g_is_split) {
            TrackList_UpdateAllExternalSurfaces();
        }
    }

    // !! rec buttons as page navigatores !!
    bool OnRecArm(MIDI_event_t* evt)
    {
        int* offset = &g_allmcus_bank_offset;
        if (g_is_split && this->m_is_split) {
            offset = &g_split_bank_offset;
        }
        int tid = evt->midi_message[1];
        int movesize = 8;
        int n {0};
        for (auto mcu : g_mcu_list) {
            if (mcu) {
                if (g_is_split && n == g_split_point_idx) {
                    movesize = 8;
                }
                if (mcu->m_offset + 8 > movesize)
                    movesize = mcu->m_offset + 8;
            }
            n++;
        }

        int newpos = (m_offset + tid) * movesize;
        int no = newpos - (newpos % movesize);

        if (no != *offset) {
            // g_allmcus_bank_offset = no;
            *offset = no;

            // update all of the sliders
            TrackList_UpdateAllExternalSurfaces();

            for (auto mcu : g_mcu_list) {
                if (this->m_is_split != mcu->m_is_split) {
                    continue;
                }
                if (mcu && mcu->m_midiout) {
                    if (mcu->m_page != 8) {
                        // not 0 .. 7
                        mcu->m_midiout->Send(
                            0x90,
                            0x0 + (mcu->m_page & 7),
                            0,
                            -1); // 0x7f : 0
                        mcu->m_page = 8;
                    }
                    if (mcu == this) {
                        mcu->m_page = tid;
                        mcu->m_midiout->Send(
                            0x90,
                            0x0 + (tid & 7),
                            0x7f,
                            -1); // 0x7f : 0
                    }
                }
                if (mcu && !mcu->m_is_mcuex && mcu->m_midiout) {
                    mcu->m_midiout->Send(
                        0xB0,
                        0x40 + 11,
                        '0' + (((*offset + 1) / 10) % 10),
                        -1);
                    mcu->m_midiout
                        ->Send(0xB0, 0x40 + 10, '0' + ((*offset + 1) % 10), -1);
                }
            }
        }
        return true;
    }

    virtual int Extended(int call, void* parm1, void* parm2, void* parm3)
    {
        DEFAULT_DEVICE_REMAP()
        if ((call == CSURF_EXT_SETSENDVOLUME || call == CSURF_EXT_SETSENDPAN) &&
            m_mode == 2) {
            auto trackid = (MediaTrack*)parm1;
            auto sendIdx = *(int*)parm2;
            auto val = *(double*)parm3;
            auto dst = (MediaTrack*)(uintptr_t)
                GetTrackSendInfo_Value(trackid, 0, sendIdx, "P_DESTTRACK");
            if (dst != GetSelectedTrack(0, 0)) {
                return 0;
            }
            if (call == CSURF_EXT_SETSENDVOLUME) {
                if (m_flipmode) {
                    SetSurfacePan(trackid, val);
                }
                else {
                    SetSurfaceVolume(trackid, val);
                }
            }
            else {
                if (m_flipmode) {
                    SetSurfaceVolume(trackid, val);
                }
                else {
                    SetSurfacePan(trackid, val);
                }
            }
        }

        return 0;
    }
};

static void parseParms(const char* str, int parms[5])
{
    parms[0] = 0;
    parms[1] = 9;
    parms[2] = parms[3] = -1;
    parms[4] = 0;

    const char* p = str;
    if (p) {
        int x = 0;
        while (x < 5) {
            while (*p == ' ')
                p++;
            if ((*p < '0' || *p > '9') && *p != '-')
                break;
            parms[x++] = atoi(p);
            while (*p && *p != ' ')
                p++;
        }
    }
}

void CSurf_MCULive::RunOutput(DWORD now)
{
    if (!m_midiout)
        return;

    if (!m_is_mcuex) {
        double pp =
            (GetPlayState() & 1) ? GetPlayPosition() : GetCursorPosition();
        char buf[2 * BUFSIZ];
        unsigned char bla[10];

        memset(bla, 0, sizeof(bla));

        int* tmodeptr =
            (int*)projectconfig_var_addr(NULL, __g_projectconfig_timemode2);

        int tmode = 0;

        if (tmodeptr && (*tmodeptr) >= 0)
            tmode = *tmodeptr & 0xff;
        else {
            tmodeptr =
                (int*)projectconfig_var_addr(NULL, __g_projectconfig_timemode);
            if (tmodeptr)
                tmode = *tmodeptr & 0xff;
        }

        if (tmode == 3) // seconds
        {
            double* toptr = (double*)projectconfig_var_addr(
                NULL,
                __g_projectconfig_timeoffs);

            if (toptr)
                pp += *toptr;
            snprintf(
                buf,
                sizeof(buf),
                "%d %02d",
                (int)pp,
                ((int)(pp * 100.0)) % 100);
            if (strlen(buf) > sizeof(bla))
                memcpy(bla, buf + strlen(buf) - sizeof(bla), sizeof(bla));
            else
                memcpy(bla + sizeof(bla) - strlen(buf), buf, strlen(buf));
        }
        else if (tmode == 4) // samples
        {
            format_timestr_pos(pp, buf, sizeof(buf), 4);
            if (strlen(buf) > sizeof(bla))
                memcpy(bla, buf + strlen(buf) - sizeof(bla), sizeof(bla));
            else
                memcpy(bla + sizeof(bla) - strlen(buf), buf, strlen(buf));
        }
        else if (tmode == 5 || tmode == 8) // frames
        {
            format_timestr_pos(pp, buf, sizeof(buf), tmode);
            char* p = buf;
            char* op = buf;
            int ccnt = 0;
            while (*p) {
                if (*p == ':') {
                    ccnt++;
                    if (tmode == 5 && ccnt != 3) {
                        p++;
                        continue;
                    }
                    *p = ' ';
                }

                *op++ = *p++;
            }
            *op = 0;
            if (strlen(buf) > sizeof(bla))
                memcpy(bla, buf + strlen(buf) - sizeof(bla), sizeof(bla));
            else
                memcpy(bla + sizeof(bla) - strlen(buf), buf, strlen(buf));
        }
        else if (tmode > 0) {
            int num_measures = 0;
            double beats = TimeMap2_timeToBeats(
                               NULL,
                               pp,
                               &num_measures,
                               NULL,
                               NULL,
                               NULL) +
                           0.000000000001;
            double nbeats = floor(beats);

            beats -= nbeats;

            int fracbeats = (int)(1000.0 * beats);

            int* measptr =
                (int*)projectconfig_var_addr(NULL, __g_projectconfig_measoffs);
            int nm = num_measures + 1 + (measptr ? *measptr : 0);
            if (nm >= 100)
                bla[0] = '0' + (nm / 100) % 10; // bars hund
            if (nm >= 10)
                bla[1] = '0' + (nm / 10) % 10; // barstens
            bla[2] = '0' + (nm) % 10;          // bars

            int nb = (int)nbeats + 1;
            if (nb >= 10)
                bla[3] = '0' + (nb / 10) % 10; // beats tens
            bla[4] = '0' + (nb) % 10;          // beats

            bla[7] = '0' + (fracbeats / 100) % 10;
            bla[8] = '0' + (fracbeats / 10) % 10;
            bla[9] = '0' + (fracbeats % 10); // frames
        }
        else {
            double* toptr = (double*)projectconfig_var_addr(
                NULL,
                __g_projectconfig_timeoffs);
            if (toptr)
                pp += (*toptr);

            int ipp = (int)pp;
            int fr = (int)((pp - ipp) * 1000.0);

            if (ipp >= 360000)
                bla[0] = '0' + (ipp / 360000) % 10; // hours hundreds
            if (ipp >= 36000)
                bla[1] = '0' + (ipp / 36000) % 10; // hours tens
            if (ipp >= 3600)
                bla[2] = '0' + (ipp / 3600) % 10; // hours

            bla[3] = '0' + (ipp / 600) % 6; // min tens
            bla[4] = '0' + (ipp / 60) % 10; // min
            bla[5] = '0' + (ipp / 10) % 6;  // sec tens
            bla[6] = '0' + (ipp % 10);      // sec
            bla[7] = '0' + (fr / 100) % 10;
            bla[8] = '0' + (fr / 10) % 10;
            bla[9] = '0' + (fr % 10); // frames
        }

        if (m_mackie_lasttime_mode != tmode) {
            m_mackie_lasttime_mode = tmode;
            m_midiout->Send(
                0x90,
                0x71,
                tmode == 5 ? 0x7F : 0,
                -1); // set smpte light
            m_midiout->Send(
                0x90,
                0x72,
                m_mackie_lasttime_mode > 0 && tmode < 3 ? 0x7F : 0,
                -1); // set beats light
        }

        {
            bool force = false;
            if (now > m_mcu_timedisp_lastforce) {
                m_mcu_timedisp_lastforce = now + 2000;
                force = true;
            }
            int x;
            for (x = 0; x < sizeof(bla); x++) {
                int idx = sizeof(bla) - x - 1;
                if (bla[idx] != m_mackie_lasttime[idx] || force) {
                    m_midiout->Send(0xB0, 0x40 + x, bla[idx], -1);
                    m_mackie_lasttime[idx] = bla[idx];
                }
            }
        }

        if (__g_projectconfig_metronome_en) {
            int* mp = (int*)projectconfig_var_addr(
                NULL,
                __g_projectconfig_metronome_en);
            int lmp = mp ? (*mp & 1) : 0;
            if ((m_last_miscstate & 1) != lmp) {
                m_last_miscstate = (m_last_miscstate & ~1) | lmp;
                m_midiout->Send(
                    0x90,
                    0x59,
                    lmp ? 0x7f : 0,
                    -1); // click (metronome) indicator
            }
        }
    }

    {
        int x;
#define VU_BOTTOM 70
        double decay = 0.0;
        if (m_mcu_meter_lastrun) {
            decay = VU_BOTTOM * (double)(now - m_mcu_meter_lastrun) /
                    (1.4 * 1000.0); // they claim 1.8s for falloff but we'll
                                    // underestimate
        }
        m_mcu_meter_lastrun = now;
        for (x = 0; x < 8; x++) {
            int idx = GetBankOffset() + x;
            MediaTrack* t;
            if ((t = CSurf_TrackFromID(idx, g_csurf_mcpmode))) {
                double pp = VAL2DB(
                    (Track_GetPeakInfo(t, 0) + Track_GetPeakInfo(t, 1)) * 0.5);

                if (m_mcu_meterpos[x] > -VU_BOTTOM * 2)
                    m_mcu_meterpos[x] -= decay;

                if (pp < m_mcu_meterpos[x])
                    continue;
                m_mcu_meterpos[x] = pp;
                int v = 0xd; // 0xe turns on clip indicator, 0xf turns it off
                if (pp < 0.0) {
                    if (pp < -VU_BOTTOM)
                        v = 0x0;
                    else
                        v = (int)((pp + VU_BOTTOM) * 13.0 / VU_BOTTOM);
                }

                m_midiout->Send(0xD0, (x << 4) | v, 0, -1);
            }
        }
    }
}

static IReaperControlSurface* createFunc(
    const char* type_string,
    const char* configString,
    int* errStats)
{
    int parms[5];
    parseParms(configString, parms);

    static bool init;
    if (!init) {
        init = true;
    }

    return new CSurf_MCULive(
        !strcmp(type_string, "MCULIVEEX"),
        parms[0],
        parms[1],
        parms[2],
        parms[3],
        parms[4],
        errStats);
}

static WDL_DLGRET dlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_INITDIALOG: {
        int parms[5];
        parseParms((const char*)lParam, parms);
        WDL_UTF8_HookComboBox(GetDlgItem(hwndDlg, IDC_COMBO2));
        WDL_UTF8_HookComboBox(GetDlgItem(hwndDlg, IDC_COMBO3));

        int n = GetNumMIDIInputs();
        int x = SendDlgItemMessage(
            hwndDlg,
            IDC_COMBO2,
            CB_ADDSTRING,
            0,
            (LPARAM)__LOCALIZE("None", "csurf"));
        SendDlgItemMessage(hwndDlg, IDC_COMBO2, CB_SETITEMDATA, x, -1);
        x = SendDlgItemMessage(
            hwndDlg,
            IDC_COMBO3,
            CB_ADDSTRING,
            0,
            (LPARAM)__LOCALIZE("None", "csurf"));
        SendDlgItemMessage(hwndDlg, IDC_COMBO3, CB_SETITEMDATA, x, -1);
        for (x = 0; x < n; x++) {
            char buf[2 * BUFSIZ];
            if (GetMIDIInputName(x, buf, sizeof(buf))) {
                int a = SendDlgItemMessage(
                    hwndDlg,
                    IDC_COMBO2,
                    CB_ADDSTRING,
                    0,
                    (LPARAM)buf);
                SendDlgItemMessage(hwndDlg, IDC_COMBO2, CB_SETITEMDATA, a, x);
                if (x == parms[2])
                    SendDlgItemMessage(hwndDlg, IDC_COMBO2, CB_SETCURSEL, a, 0);
            }
        }
        n = GetNumMIDIOutputs();
        for (x = 0; x < n; x++) {
            char buf[2 * BUFSIZ];
            if (GetMIDIOutputName(x, buf, sizeof(buf))) {
                int a = SendDlgItemMessage(
                    hwndDlg,
                    IDC_COMBO3,
                    CB_ADDSTRING,
                    0,
                    (LPARAM)buf);
                SendDlgItemMessage(hwndDlg, IDC_COMBO3, CB_SETITEMDATA, a, x);
                if (x == parms[3])
                    SendDlgItemMessage(hwndDlg, IDC_COMBO3, CB_SETCURSEL, a, 0);
            }
        }
        SetDlgItemInt(hwndDlg, IDC_EDIT1, parms[0], TRUE);
        SetDlgItemInt(hwndDlg, IDC_EDIT2, parms[1], FALSE);
        if (parms[4] & CONFIG_FLAG_FADER_TOUCH_MODE)
            CheckDlgButton(hwndDlg, IDC_CHECK1, BST_CHECKED);

    } break;
    case WM_USER + 1024:
        if (wParam > 1 && lParam) {
            char tmp[2 * BUFSIZ];

            int indev = -1, outdev = -1, offs = 0, size = 9;
            int r = SendDlgItemMessage(hwndDlg, IDC_COMBO2, CB_GETCURSEL, 0, 0);
            if (r != CB_ERR)
                indev = SendDlgItemMessage(
                    hwndDlg,
                    IDC_COMBO2,
                    CB_GETITEMDATA,
                    r,
                    0);
            r = SendDlgItemMessage(hwndDlg, IDC_COMBO3, CB_GETCURSEL, 0, 0);
            if (r != CB_ERR)
                outdev = SendDlgItemMessage(
                    hwndDlg,
                    IDC_COMBO3,
                    CB_GETITEMDATA,
                    r,
                    0);

            BOOL t;
            r = GetDlgItemInt(hwndDlg, IDC_EDIT1, &t, TRUE);
            if (t)
                offs = r;
            r = GetDlgItemInt(hwndDlg, IDC_EDIT2, &t, FALSE);
            if (t) {
                if (r < 1)
                    r = 1;
                else if (r > 256)
                    r = 256;
                size = r;
            }
            int cflags = 0;

            snprintf(
                tmp,
                sizeof(tmp),
                "%d %d %d %d %d",
                offs,
                size,
                indev,
                outdev,
                cflags);
            lstrcpyn((char*)lParam, tmp, wParam);
        }
        break;
    }
    return 0;
}

static HWND configFunc(
    const char* type_string,
    HWND parent,
    const char* initConfigString)
{
    return CreateDialogParam(
        g_hInst,
        MAKEINTRESOURCE(IDD_SURFACEEDIT_MCU1),
        parent,
        dlgProc,
        (LPARAM)initConfigString);
}

reaper_csurf_reg_t csurf_mcu_reg = {
    "MCULIVE",
    // !WANT_LOCALIZE_STRINGS_BEGIN:csurf_type
    "MCU Live",
    // !WANT_LOCALIZE_STRINGS_END
    createFunc,
    configFunc,
};
reaper_csurf_reg_t csurf_mcuex_reg = {
    "MCULIVEEX",
    // !WANT_LOCALIZE_STRINGS_BEGIN:csurf_type
    "MCU Live Extender",
    // !WANT_LOCALIZE_STRINGS_END
    createFunc,
    configFunc,
};

const char* defstring_Map =
    "void\0int,int,int,isRemap\0"
    "device,button,command_id,bool\0"
    "Maps MCU Live device# button to REAPER command ID. E.g. "
    "reaper.MCULive_Map(0,0x5b, 40340) maps MCU Rewind to \"Track: Unsolo "
    "all "
    "tracks\".\n" //
    "\n"          //
    "MCU buttons:\n"

    "0x0x : rec arm push x=0..7 (vv:..)\n "
    "0x0x : solo push x=8..F (vv:..)\n "
    "0x1x : mute push x=0..7 (vv:..)\n "
    "0x1x : selected push x=8..F (vv:..)\n "
    "0x2x : pan knob push, x=0..7 (vv:..)\n "
    "0x28 : assignment track \n"
    "0x29 : assignment send \n"
    "0x2a : assignment pan/surround \n"
    "0x2b : assignment plug-in \n"
    "0x2c : assignment eq \n"
    "0x2d : assignment instrument \n"
    "0x2e : bank down button  \n"
    "0x2f : channel down button \n"
    "0x30 : bank up button \n"
    "0x31 : channel up button \n"
    "0x32 : flip button \n"
    "0x33 : global view button \n"
    "0x34 : name/value display button \n"
    "0x35 : smpte/beats mode switch \n"
    "0x36 : f1 \n"
    "0x37 : f2 \n"
    "0x38 : f3 \n"
    "0x39 : f4 \n"
    "0x3a : f5 \n"
    "0x3b : f6 \n"
    "0x3c : f7 \n"
    "0x3d : f8 \n"
    "0x3e : global view : midi tracks \n"
    "0x3f : global view : inputs \n"
    "0x40 : global view : audio tracks \n"
    "0x41 : global view : audio instrument \n"
    "0x42 : global view : aux \n"
    "0x43 : global view : busses \n"
    "0x44 : global view : outputs \n"
    "0x45 : global view : user \n"
    "0x46 : shift modifier \n"
    "0x47 : option modifier \n"
    "0x48 : control modifier \n"
    "0x49 : alt modifier \n"
    "0x4a : automation read/off \n"
    "0x4b : automation write \n"
    "0x4c : automation trim \n"
    "0x4d : automation touch \n"
    "0x4e : automation latch \n"
    "0x4f : automation group \n"
    "0x50 : utilities save \n"
    "0x51 : utilities undo \n"
    "0x52 : utilities cancel \n"
    "0x53 : utilities enter \n"
    "0x54 : marker \n"
    "0x55 : nudge \n"
    "0x56 : cycle \n"
    "0x57 : drop \n"
    "0x58 : replace \n"
    "0x59 : click \n"
    "0x5a : solo \n"
    "0x5b : transport rewind \n"
    "0x5c : transport ffwd \n"
    "0x5d : transport pause \n"
    "0x5e : transport play \n"
    "0x5f : transport record \n"
    "0x60 : up arrow button  \n"
    "0x61 : down arrow button  \n"
    "0x62 : left arrow button  \n"
    "0x63 : right arrow button \n"
    "0x64 : zoom button \n"
    "0x65 : scrub button \n"
    "0x6x : fader touch x=8..f \n"
    "0x70 : master fader touch \n";

void Map(int device, int button, int command_id, bool isRemap)
{
    // if (isRemap) {
    //     g_mcu_list.Get(device)->m_button_remap[button] = command_id;
    // }
    // else {
    //     g_mcu_list.Get(device)->m_button_map[button] = command_id;
    // }
    return;
}
const char* defstring_SetButtonPressOnly =
    "void\0int,int,bool\0"
    "device,button,isSet\0"
    "Button functions as press only by default. Set false for press and "
    "release function.";

void SetButtonPressOnly(int device, int button, bool isSet)
{
    // g_mcu_list.Get(device)->m_press_only_buttons[button] = isSet ? 1 : 0;
    return;
}

const char* defstring_GetIsButtonPressed =
    "bool\0int,int\0"
    "device,button\0"
    "Returns true if device# button# is currently pressed.";

bool GetIsButtonPressed(int device, int button)
{
    // return g_mcu_list.Get(device)->m_button_pressed[button] ? true : false;
    return true;
}

const char* defstring_GetFaderValue =
    "double\0int,int,int\0"
    "device,faderIdx,param\0"
    "Returns zero-indexed fader parameter value. 0 = lastpos, 1 = "
    "lasttouch, 2 "
    "= any lastmove";

double GetFaderValue(int device, int faderIdx, int param)
{
    // if (!(faderIdx < g_mcu_list.Get(device)->m_size)) {
    //     return -1;
    // }
    // if (param == 0) {
    //     return g_mcu_list.Get(device)->m_vol_lastpos[faderIdx];
    // }
    // if (param == 1) {
    //     return g_mcu_list.Get(device)->m_fader_lasttouch[faderIdx];
    // }
    // if (param == 2) {
    //     return g_mcu_list.Get(device)->m_fader_lastmove;
    // }
    return -1;
}

void RegisterAPI()
{
    plugin_register("API_MCULive_Map", (void*)&Map);
    plugin_register("APIdef_MCULive_Map", (void*)defstring_Map);
    plugin_register(
        "APIvararg_MCULive_Map",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&Map>));

    plugin_register(
        "API_MCULive_SetButtonPressOnly",
        (void*)&SetButtonPressOnly);
    plugin_register(
        "APIdef_MCULive_SetButtonPressOnly",
        (void*)defstring_SetButtonPressOnly);
    plugin_register(
        "APIvararg_MCULive_SetButtonPressOnly",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SetButtonPressOnly>));

    plugin_register(
        "API_MCULive_GetIsButtonPressed",
        (void*)&GetIsButtonPressed);
    plugin_register(
        "APIdef_MCULive_GetIsButtonPressed",
        (void*)defstring_GetIsButtonPressed);
    plugin_register(
        "APIvararg_MCULive_GetIsButtonPressed",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&GetIsButtonPressed>));

    plugin_register("API_MCULive_GetFaderValue", (void*)&GetFaderValue);
    plugin_register(
        "APIdef_MCULive_GetFaderValue",
        (void*)defstring_GetFaderValue);
    plugin_register(
        "APIvararg_MCULive_GetFaderValue",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&GetFaderValue>));
    return;
}

} // namespace ReaMCULive