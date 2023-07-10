/*
** reaper_csurf
** MCU support
** Copyright (C) 2006-2008 Cockos Incorporated
** License: LGPL.
*/
#include "reascript_vararg.hpp"

#include <algorithm>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "reaper_plugin_functions.h"

#include "csurf.h"

// #define timeGetTime() GetTickCount64()

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
static int g_mode_is_global {}; // mask for global modes

static int g_is_split {0};
static int g_split_offset {0};
static int g_split_point_idx {0}; // surface split point device index

std::mutex g_mutex;

typedef void (CSurf_MCULive::*ScheduleFunc)();

// struct ScheduledAction {
//     ScheduledAction(double time, ScheduleFunc func)
//     {
//         this->next = NULL;
//         this->time = time;
//         this->func = func;
//     }

//     ScheduledAction* next;
//     double time;
//     ScheduleFunc func;
// };

#define CONFIG_FLAG_FADER_TOUCH_MODE 1

#define DOUBLE_CLICK_INTERVAL 0.250 /* ms */
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
    bool m_is_default {true};
    int m_midi_in_dev;
    int m_midi_out_dev;
    int m_offset;
    int m_offset_orig;
    int m_size;
    int m_flipmode {};
    midi_Output* m_midiout;
    midi_Input* m_midiin;

    int m_vol_lastpos[BUFSIZ];
    int m_pan_lastpos[BUFSIZ];
    char m_mackie_lasttime[10];
    int m_mackie_lasttime_mode;
    int m_mackie_modifiers;
    int m_cfg_flags;      // CONFIG_FLAG_FADER_TOUCH_MODE etc
    int m_last_miscstate; // &1=metronome

    int m_fader_pos[BUFSIZ] {};
    int m_encoder_pos[BUFSIZ] {};

    int m_button_map[BUFSIZ] {};
    int m_buttons_passthrough[BUFSIZ] {};
    int m_press_only_buttons[BUFSIZ] {};
    int m_button_states[BUFSIZ] {};
    int m_button_remap[BUFSIZ] {};

    std::vector<MIDI_event_t> midiBuffer {};

    int m_page {};
    int m_mode {};            // mode assignment
    int m_modemask {};        // mode assignment mask
    int m_flipflags {1 << 0}; // allow flipmode flags
    int m_is_split;

    char m_fader_touchstate[BUFSIZ];
    double m_fader_lasttouch[BUFSIZ]; // m_fader_touchstate changes will
                                      // clear this, moves otherwise set it.
                                      // if set to -1, then totally disabled
    double m_pan_lasttouch[BUFSIZ];

    WDL_String m_descspace;
    char m_configtmp[4 * BUFSIZ];

    double m_mcu_meterpos[8];
    double m_mcu_timedisp_lastforce {0};
    double m_mcu_meter_lastrun {0};
    int m_mackie_arrow_states;
    double m_buttonstate_lastrun {0};
    double m_frameupd_lastrun {0};
    // ScheduledAction* m_schedule;
    // SelectedTrack* m_selected_tracks;

// If user accidentally hits fader, we want to wait for user
// to stop moving fader and then reset it to it's orginal position
#define FADER_REPOS_WAIT 0.250
    bool m_repos_faders;
    double m_fader_lastmove;

    int m_button_last;
    double m_button_last_time;

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
            m_fader_lastmove = time_precise();

            int tid = evt->midi_message[0] & 0xf;

            auto msb = evt->midi_message[2];
            auto lsb = evt->midi_message[1];
            auto faderVal = lsb | (msb << 7);

            if (tid >= 0 && tid < 9 && m_fader_lasttouch[tid] != 0xffffffff)
                m_fader_lasttouch[tid] = m_fader_lastmove;

            if (!m_is_default) {
                if ((m_cfg_flags & CONFIG_FLAG_FADER_TOUCH_MODE) &&
                    !m_fader_touchstate[tid]) {
                    m_repos_faders = true;
                }
                else if (m_fader_pos[tid] != faderVal) {
                    m_fader_pos[tid] = faderVal;
                    m_midiout->Send(
                        0xe0 + (tid & 0xf),
                        faderVal & 0x7f,
                        (faderVal >> 7) & 0x7f,
                        -1);
                }
                return true;
            }

            if (tid == 8)
                tid = 0; // master offset, master=0
            else
                tid += GetBankOffset();

            MediaTrack* tr = CSurf_TrackFromID(tid, g_csurf_mcpmode);

            if (tr) {
                if ((m_cfg_flags & CONFIG_FLAG_FADER_TOUCH_MODE) &&
                    !GetTouchState(tr)) {
                    m_repos_faders = true;
                    return true;
                }

                m_fader_pos[tid - GetBankOffset() ? tid - GetBankOffset() : 8] =
                    faderVal;

                double val {0};
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

            m_pan_lasttouch[tid & 7] = time_precise();

            if (evt->midi_message[2] & 0x40) {
                m_encoder_pos[tid & 7] = -(evt->midi_message[2] & 0x3f);
            }
            else {
                m_encoder_pos[tid & 7] = evt->midi_message[2] & 0x3f;
            }

            if (!m_is_default) {
                return true;
            }

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

            if (!m_is_default) {
                return true;
            }

            return true;
        }
        return false;
    }

    bool OnRotaryEncoderPush(MIDI_event_t* evt)
    {
        int trackid = evt->midi_message[1] - 0x20;
        m_pan_lasttouch[trackid] = time_precise();

        if (!m_is_default) {
            return true;
        }

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

        if (g_mcu_list.size() == 1) {
            g_mode_is_global = (1 << 8) - 1;
            g_split_point_idx = ~0;
        }

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

                if (mcu->m_flipmode &&
                    !(m_flipflags & 1 << (mcu->m_mode - 1))) {
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

    bool OnKeyModifier(MIDI_event_t* evt)
    {
        int mask = (1 << (evt->midi_message[1] - 0x46));
        if (evt->midi_message[2] >= 0x40)
            m_mackie_modifiers |= mask;
        else
            m_mackie_modifiers &= ~mask;
        return true;
    }

    bool OnScroll(MIDI_event_t* evt)
    {
        if (evt->midi_message[2] > 0x40)
            m_mackie_arrow_states |= 1 << (evt->midi_message[1] - 0x60);
        else
            m_mackie_arrow_states &= ~(1 << (evt->midi_message[1] - 0x60));
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

        m_button_states[evt->midi_message[1]] = evt->midi_message[2];

        unsigned int evt_code =
            evt->midi_message[1];              // get_midi_evt_code( evt );

        if (m_buttons_passthrough[evt_code]) { // Pass thru if not otherwise
                                               // handled
            if (evt->midi_message[2] >= 0x40) {
                int a = evt->midi_message[1];
                MIDI_event_t evt = {
                    0,
                    3,
                    {(unsigned char)(0xbf - (m_mackie_modifiers & 15)),
                     (unsigned char)a,
                     0}};
                kbd_OnMidiEvent(&evt, -1);
                return true;
            }
            else if (!m_press_only_buttons[evt_code]) {
                int a = evt->midi_message[1];
                MIDI_event_t evt = {
                    0,
                    3,
                    {(unsigned char)(0xbf - (m_mackie_modifiers & 15)),
                     (unsigned char)a,
                     0}};
                kbd_OnMidiEvent(&evt, -1);
                return true;
            }
        }

        if (!m_is_default) {
            return true;
        }

        static const int nHandlers = 11;
        static const int nPressOnlyHandlers = 5;
        static const ButtonHandler handlers[nHandlers] = {
            //
            {0x00, 0x07, &CSurf_MCULive::OnRecArm, NULL},
            {0x08, 0x0f, NULL, &CSurf_MCULive::OnSoloDC},
            {0x08, 0x17, &CSurf_MCULive::OnMuteSolo, NULL},
            {0x18,
             0x1f,
             &CSurf_MCULive::OnChannelSelectDC,
             &CSurf_MCULive::OnChannelSelect},
            {0x2e, 0x31, &CSurf_MCULive::OnBankChannel, NULL},
            // Press and release events
            {0x32, 0x45, &CSurf_MCULive::OnMCULiveButton, NULL},
            {0x4a, 0x5f, &CSurf_MCULive::OnMCULiveButton, NULL},
            {0x64, 0x67, &CSurf_MCULive::OnMCULiveButton, NULL},
            {0x46, 0x49, &CSurf_MCULive::OnKeyModifier},
            {0x60, 0x63, &CSurf_MCULive::OnScroll},
            {0x68, 0x70, &CSurf_MCULive::OnTouch},
        };

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
            double now = time_precise(); // timeGetTime();
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
        g_mode_is_global = (1 << 1) - 1; // mask for global modes
        g_split_point_idx = (int)g_mcu_list.size() - 1;

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
        memset(m_buttons_passthrough, 1, sizeof(m_buttons_passthrough));
        memset(m_press_only_buttons, 1, sizeof(m_press_only_buttons));
        memset(m_button_states, 0, sizeof(m_button_states));

        // m_button_remap[0x32] = 0x29; // flip to sends
        for (int i = 0; i <= 0x32; i++) {
            m_buttons_passthrough[i] = 0;
        }

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
        // m_schedule = NULL;
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
        // while (m_schedule != NULL) {
        //     ScheduledAction* temp = m_schedule;
        //     m_schedule = temp->next;
        //     delete temp;
        // }
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

    void RunOutput(double now);

    void Run()
    {
        auto now = time_precise(); // timeGetTime();
        auto x = now - m_frameupd_lastrun;
        auto y = 1. / std::max((*g_config_csurf_rate), 1);

        if (!m_is_default) {
            return;
        }

        if (x >= y) {
            m_frameupd_lastrun = now;

            RunOutput(now);
        }

        if (m_midiin) {
            if (midiBuffer.size() > BUFSIZ) {
                midiBuffer.clear();
            }
            m_midiin->SwapBufsPrecise(0, now);
            int l = 0;
            MIDI_eventlist* list = m_midiin->GetReadBuf();
            MIDI_event_t* evts;
            while ((evts = list->EnumItems(&l))) {
                midiBuffer.push_back(*evts);
                if (m_is_default) {
                    OnMIDIEvent(evts);
                }
            }
            if (m_mackie_arrow_states) {
                if ((now - m_buttonstate_lastrun) >= 0.1) {
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
        if (!m_is_default) {
            return;
        }
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
        if (!m_is_default) {
            return;
        }
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
        if (!m_is_default) {
            return;
        }
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
        if (!m_is_default) {
            return;
        }

        FIXID(id)
        if (m_midiout && id >= 0 && id < 256 && id < m_size) {
            if (id < 8)
                m_midiout->Send(0x90, 0x18 + (id & 7), selected ? 0x7f : 0, -1);
        }
    }

    void SetSurfaceSolo(MediaTrack* trackid, bool solo)
    {
        if (!m_is_default) {
            return;
        }
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
        if (!m_is_default) {
            return;
        }
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
                    (time_precise() - m_pan_lasttouch[id]) <
                        3) // fake touch, go for 3s after last movement
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
                if ((time_precise() - m_fader_lasttouch[id]) < 3)
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

    bool OnBankChannel(MIDI_event_t* evt)
    {
        int* offset = &g_allmcus_bank_offset;
        if (g_is_split && this->m_is_split) {
            offset = &g_split_bank_offset;
        }

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
        int maxfaderpos = 0;

        for (auto item : g_mcu_list) {
            if (item) {
                if (item->m_offset + movesize > maxfaderpos)
                    maxfaderpos = item->m_offset + movesize;
            }
        }

        // if (evt->midi_message[1] >= 0x30)
        //     movesize = 1;
        // else
        //     movesize = 8; // maxfaderpos?

        if (evt->midi_message[1] & 1) // increase by X
        {
            int msize = CSurf_NumTracks(g_csurf_mcpmode);
            if (movesize > 1) {
                if (*offset + maxfaderpos >= msize)
                    return true;
            }

            *offset += movesize;

            if (*offset >= msize)
                *offset = msize - 1;
        }
        else {
            if (*offset == 0) {
                return true;
            }
            *offset -= movesize;
            if (*offset < 0)
                *offset = 0;
        }
        // update all of the sliders

        auto newPage = *offset / movesize;
        auto device = newPage / 8;
        newPage = newPage % 8;

        n = 0;
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
                if (device == n) {
                    mcu->m_page = newPage;
                    mcu->m_midiout->Send(
                        0x90,
                        0x0 + (m_page & 7),
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
            n++;
        }
        TrackList_UpdateAllExternalSurfaces();
        return true;
    }
    void OnTrackSelection(MediaTrack* trackid)
    {

        if (!m_is_default) {
            return;
        }
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

        if (!m_is_default) {
            return true;
        }
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

void CSurf_MCULive::RunOutput(double now)
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
                bla[1] = '0' + (nm / 10) % 10;  // barstens
            bla[2] = '0' + (nm) % 10;           // bars

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
                bla[1] = '0' + (ipp / 36000) % 10;  // hours tens
            if (ipp >= 3600)
                bla[2] = '0' + (ipp / 3600) % 10;   // hours

            bla[3] = '0' + (ipp / 600) % 6;         // min tens
            bla[4] = '0' + (ipp / 60) % 10;         // min
            bla[5] = '0' + (ipp / 10) % 6;          // sec tens
            bla[6] = '0' + (ipp % 10);              // sec
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
                    (1.4); // they claim 1.8s for falloff but we'll
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

static const char* defstring_Map =
    "int\0int,int,int,bool\0"
    "device,button,command_id,isRemap\0"
    "Maps MCU Live device# button# to REAPER command ID. E.g. "
    "reaper.MCULive_Map(0,0x5b, 40340) maps MCU Rewind to \"Track: Unsolo all "
    "tracks\". " //
    "Or remap button to another button if your MCU button layout doesnt play "
    "nicely with default MCULive mappings. "
    "By default range 0x00 .. 0x2d is in use. "
    "Button numbers are second column (prefixed with 0x) e.g. '90 5e' 0x5e for "
    "'transport : play', roughly. \n"
    "\n"
    "mcu documentation: \n"
    "mcu=>pc: \n"
    "  the mcu seems to send, when it boots (or is reset) f0 00 00 66 14 01 58 "
    "59 5a 57 18 61 05 57 18 61 05 f7 \n"
    "  ex vv vv    :   volume fader move, x=0..7, 8=master, vv vv is int14 \n"
    "  b0 1x vv    :   pan fader move, x=0..7, vv has 40 set if negative, low "
    "bits 0-31 are move amount \n"
    "  b0 3c vv    :   jog wheel move, 01 or 41 \n"
    "  to the extent the buttons below have leds, you can set them by sending "
    "these messages, with 7f for on, 1 for blink, 0 for off. \n"
    "  90 0x vv    :   rec arm push x=0..7 (vv:..) \n"
    "  90 0x vv    :   solo push x=8..f (vv:..) \n"
    "  90 1x vv    :   mute push x=0..7 (vv:..) \n"
    "  90 1x vv    :   selected push x=8..f (vv:..) \n"
    "  90 2x vv    :   pan knob push, x=0..7 (vv:..) \n"
    "  90 28 vv    :   assignment track \n"
    "  90 29 vv    :   assignment send \n"
    "  90 2a vv    :   assignment pan/surround \n"
    "  90 2b vv    :   assignment plug-in \n"
    "  90 2c vv    :   assignment eq \n"
    "  90 2d vv    :   assignment instrument \n"
    "  90 2e vv    :   bank down button (vv: 00=release, 7f=push) \n"
    "  90 2f vv    :   channel down button (vv: ..) \n"
    "  90 30 vv    :   bank up button (vv:..) \n"
    "  90 31 vv    :   channel up button (vv:..) \n"
    "  90 32 vv    :   flip button \n"
    "  90 33 vv    :   global view button \n"
    "  90 34 vv    :   name/value display button \n"
    "  90 35 vv    :   smpte/beats mode switch (vv:..) \n"
    "  90 36 vv    :   f1 \n"
    "  90 37 vv    :   f2 \n"
    "  90 38 vv    :   f3 \n"
    "  90 39 vv    :   f4 \n"
    "  90 3a vv    :   f5 \n"
    "  90 3b vv    :   f6 \n"
    "  90 3c vv    :   f7 \n"
    "  90 3d vv    :   f8 \n"
    "  90 3e vv    :   global view : midi tracks \n"
    "  90 3f vv    :   global view : inputs \n"
    "  90 40 vv    :   global view : audio tracks \n"
    "  90 41 vv    :   global view : audio instrument \n"
    "  90 42 vv    :   global view : aux \n"
    "  90 43 vv    :   global view : busses \n"
    "  90 44 vv    :   global view : outputs \n"
    "  90 45 vv    :   global view : user \n"
    "  90 46 vv    :   shift modifier (vv:..) \n"
    "  90 47 vv    :   option modifier \n"
    "  90 48 vv    :   control modifier \n"
    "  90 49 vv    :   alt modifier \n"
    "  90 4a vv    :   automation read/off \n"
    "  90 4b vv    :   automation write \n"
    "  90 4c vv    :   automation trim \n"
    "  90 4d vv    :   automation touch \n"
    "  90 4e vv    :   automation latch \n"
    "  90 4f vv    :   automation group \n"
    "  90 50 vv    :   utilities save \n"
    "  90 51 vv    :   utilities undo \n"
    "  90 52 vv    :   utilities cancel \n"
    "  90 53 vv    :   utilities enter \n"
    "  90 54 vv    :   marker \n"
    "  90 55 vv    :   nudge \n"
    "  90 56 vv    :   cycle \n"
    "  90 57 vv    :   drop \n"
    "  90 58 vv    :   replace \n"
    "  90 59 vv    :   click \n"
    "  90 5a vv    :   solo \n"
    "  90 5b vv    :   transport rewind (vv:..) \n"
    "  90 5c vv    :   transport ffwd (vv:..) \n"
    "  90 5d vv    :   transport pause (vv:..) \n"
    "  90 5e vv    :   transport play (vv:..) \n"
    "  90 5f vv    :   transport record (vv:..) \n"
    "  90 60 vv    :   up arrow button  (vv:..) \n"
    "  90 61 vv    :   down arrow button 1 (vv:..) \n"
    "  90 62 vv    :   left arrow button 1 (vv:..) \n"
    "  90 63 vv    :   right arrow button 1 (vv:..) \n"
    "  90 64 vv    :   zoom button (vv:..) \n"
    "  90 65 vv    :   scrub button (vv:..) \n"
    "  90 6x vv    :   fader touch x=8..f \n"
    "  90 70 vv    :   master fader touch \n"
    "pc=>mcu: \n"
    "  f0 00 00 66 14 12 xx <data> f7   : update lcd. xx=offset (0-112), "
    "string. display is 55 chars wide, second line begins at 56, though. \n"
    "  f0 00 00 66 14 08 00 f7          : reset mcu \n"
    "  f0 00 00 66 14 20 0x 03 f7       : put track in vu meter mode, x=track  "
    " \n"
    "  90 73 vv : rude solo light (vv: 7f=on, 00=off, 01=blink) \n"
    "  b0 3x vv : pan display, x=0..7, vv=1..17 (hex) or so \n"
    "  b0 4x vv : right to left of leds. if 0x40 set in vv, dot below char is "
    "set (x=0..11) \n"
    "  d0 yx    : update vu meter, y=track, x=0..d=volume, e=clip on, f=clip "
    "off \n"
    "  ex vv vv : set volume fader, x=track index, 8=master \n";

int Map(int device, int button, int command_id, bool isRemap)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || button < 0 ||
        button >= BUFSIZ) {
        return -1;
    }
    if (isRemap) {
        g_mcu_list[device]->m_button_remap[button] = command_id;
    }
    else {
        g_mcu_list[device]->m_button_map[button] = command_id;
    }
    return button;
}
const char* defstring_SetButtonPressOnly =
    "int\0int,int,bool\0"
    "device,button,isSet\0"
    "Buttons function as press only by default. Set false for press and "
    "release function.";

int SetButtonPressOnly(int device, int button, bool isSet)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || button < 0 ||
        button >= BUFSIZ) {
        return -1;
    }
    g_mcu_list[device]->m_press_only_buttons[button] = isSet ? 1 : 0;
    return button;
}

const char* defstring_SetButtonPassthrough =
    "int\0int,int,bool\0"
    "device,button,isSet\0"
    "Set button as MIDI passthrough.";

int SetButtonPassthrough(int device, int button, bool isSet)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || button < 0 ||
        button >= BUFSIZ) {
        return -1;
    }
    g_mcu_list[device]->m_buttons_passthrough[button] = isSet ? 1 : 0;
    return button;
}

const char* defstring_SetDefault =
    "void\0int,bool\0"
    "device,isSet\0"
    "Enables/disables default out-of-the-box operation.";

void SetDefault(int device, bool isSet)
{
    if (device < 0 || device >= (int)g_mcu_list.size()) {
        return;
    }
    g_mcu_list[device]->m_is_default = isSet;
    return;
}

const char* defstring_SetDisplay =
    "void\0int,int,const char*,int\0"
    "device,pos,message,pad\0"
    "Write to display. 112 characters, 56 per row.";

void SetDisplay(int device, int pos, const char* message, int pad)
{
    if (device < 0 || device >= (int)g_mcu_list.size()) {
        return;
    }
    g_mcu_list[device]->UpdateMackieDisplay(pos, message, pad);
    return;
}

const char* defstring_SetOption =
    "void\0int,int\0"
    "option,value\0"
    "1 : surface split point device index \n"
    "2 : 'mode-is-global' bitmask/flags, first 6 bits";

void SetOption(int option, int value)
{
    if (option > 2 || option < 1) {
        return;
    }
    if (option == 1) {
        g_split_point_idx = value < (int)g_mcu_list.size() ? value : -1;
    }
    if (option == 2) {
        g_mode_is_global = value & ((1 << 8) - 1);
    }

    return;
}

static const char* defstring_GetButtonValue =
    "int\0int,int\0"
    "device,button\0"
    "Get current button state.";

static int GetButtonValue(int device, int button)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || button < 0 ||
        button >= BUFSIZ) {
        return -1;
    }
    return g_mcu_list[device]->m_button_states[button];
}

static const char* defstring_SetButtonValue =
    "int\0int,int,int\0"
    "device,button,value\0"
    "Set button led/mode/state. Value 0 = off,1 = blink, 0x7f = on, usually.";

static int SetButtonValue(int device, int button, int value)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || button < 0 ||
        button >= BUFSIZ) {
        return -1;
    }
    if (!g_mcu_list[device]->m_midiout)
        return -1;

    g_mcu_list[device]->m_midiout->Send(0x90, button, value, -1);
    return value;
}

static const char* defstring_GetFaderValue =
    "double\0int,int,int\0"
    "device,faderIdx,param\0"
    "Returns zero-indexed fader parameter value. 0 = lastpos, 1 = "
    "lasttouch, 2 "
    "= lastmove (any fader)";

static double GetFaderValue(int device, int faderIdx, int param)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || faderIdx < 0 ||
        faderIdx >= BUFSIZ) {
        return -1;
    }
    if (param == 0) {
        return (double)g_mcu_list[device]->m_fader_pos[faderIdx];
    }
    if (param == 1) {
        return g_mcu_list[device]->m_fader_lasttouch[faderIdx];
    }
    if (param == 2) {
        return g_mcu_list[device]->m_fader_lastmove;
    }
    return -1;
}

static const char* defstring_GetEncoderValue =
    "double\0int,int,int\0"
    "device,encIdx,param\0"
    "Returns zero-indexed encoder parameter value. 0 = lastpos, 1 = "
    "lasttouch";

static double GetEncoderValue(int device, int encIdx, int param)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || encIdx < 0 ||
        encIdx >= BUFSIZ) {
        return -1;
    }
    if (param == 0) {
        return (double)g_mcu_list[device]->m_encoder_pos[encIdx];
    }
    if (param == 1) {
        return g_mcu_list[device]->m_pan_lasttouch[encIdx];
    }
    return -1;
}

static const char* defstring_SetFaderValue =
    "int\0int,int,double,int\0"
    "device,faderIdx,val,type\0"
    "Set fader to value 0 ... 1.0. Type 0 = linear, 1 = track "
    "volume, 2 = pan. Returns scaled value.";

static int SetFaderValue(int device, int faderIdx, double val, int type)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || faderIdx < 0 ||
        faderIdx >= BUFSIZ || val < 0 || val > 1) {
        return -1;
    }
    if (!g_mcu_list[device]->m_midiout)
        return -1;
    int newVal {-1};
    if (type == 0) {
        newVal = (int)val * 16383;
    }
    if (type == 1) {
        newVal = volToInt14(val);
    }
    if (type == 2) {
        newVal = panToInt14((val - 0.5) * 2);
    }
    if (newVal == -1) {
        return -1;
    }

    g_mcu_list[device]->m_midiout->Send(
        0xe0 + (faderIdx & 0xf),
        newVal & 0x7f,
        (newVal >> 7) & 0x7f,
        -1);
    return newVal;
}

static const char* defstring_SetEncoderValue =
    "int\0int,int,double,int\0"
    "device,encIdx,val,type\0"
    "Set encoder to value 0 ... 1.0. Type 0 = linear, 1 = track "
    "volume, 2 = pan. Returns scaled value.";

static int SetEncoderValue(int device, int encIdx, double val, int type)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || encIdx < 0 ||
        encIdx >= BUFSIZ || val < 0 || val > 1) {
        return -1;
    }
    if (!g_mcu_list[device]->m_midiout)
        return -1;
    int newVal {-1};
    if (type == 0) {
        newVal = (unsigned char)(val * 127);
    }
    if (type == 1) {
        newVal = volToChar(val);
    }
    if (type == 2) {
        newVal = panToChar((val - 0.5) * 2);
    }
    if (newVal == -1) {
        return -1;
    }

    if (encIdx < 8)
        g_mcu_list[device]->m_midiout->Send(
            0xb0,
            0x30 + (encIdx & 0xf),
            1 + ((newVal * 11) >> 7),
            -1);
    return newVal;
}

static const char* defstring_SetMeterValue =
    "int\0int,int,double,int\0"
    "device,meterIdx,val,type\0"
    "Set meter value 0 ... 1.0. Type 0 = linear, 1 = track "
    "volume (with decay).";

static int SetMeterValue(int device, int meterIdx, double val, int type)
{
    if (device < 0 || device >= (int)g_mcu_list.size() || meterIdx < 0 ||
        meterIdx >= BUFSIZ || val < 0 || val > 1) {
        return -1;
    }
    if (!g_mcu_list[device]->m_midiout)
        return -1;
    // double pp =
    //     VAL2DB((Track_GetPeakInfo(t, 0) + Track_GetPeakInfo(t, 1)) * 0.5);

    int v {0};
    auto pp = val;
    auto now = time_precise();
    g_mcu_list[device]->m_mcu_meter_lastrun = now;
    if (type == 1) {
        auto decay = 0.0;
        pp = VAL2DB(val);
        if (g_mcu_list[device]->m_mcu_meter_lastrun) {
            decay = VU_BOTTOM *
                    (double)(now - g_mcu_list[device]->m_mcu_meter_lastrun) /
                    (1.4); // * 1000.0); // they claim 1.8s for falloff but
                           // we'll underestimate
        }
        if (g_mcu_list[device]->m_mcu_meterpos[meterIdx] > -VU_BOTTOM * 2)
            g_mcu_list[device]->m_mcu_meterpos[meterIdx] -= decay;
        if (pp < g_mcu_list[device]->m_mcu_meterpos[meterIdx])
            pp = g_mcu_list[device]->m_mcu_meterpos[meterIdx];
        g_mcu_list[device]->m_mcu_meterpos[meterIdx] = pp;
        v = 0xd; // 0xe turns on clip indicator, 0xf turns it off
        if (pp < 0.0) {
            if (pp < -VU_BOTTOM)
                v = 0x0;
            else
                v = (int)((pp + VU_BOTTOM) * 13.0 / VU_BOTTOM);
        }
    }
    else {
        v = (int)(val * 16);
    }

    g_mcu_list[device]->m_midiout->Send(0xD0, (meterIdx << 4) | v, 0, -1);
    return v;
}

static const char* defstring_Reset =
    "int\0int\0"
    "device\0"
    "Reset device. device < 0 resets all and returns number of devices.";
static int Reset(int device)
{
    if (device >= (int)g_mcu_list.size()) {
        return -1;
    }
    if (device < 0) {
        for (auto&& i : g_mcu_list) {
            i->MCUReset();
        }
        return (int)g_mcu_list.size();
    }
    g_mcu_list[device]->MCUReset();
    return device;
}

static const char* defstring_GetDevice =
    "int\0int,int\0"
    "device,type\0"
    "Get MIDI input or output dev ID. type 0 is input dev, type 1 is output "
    "dev. device < 0 returns number of MCULive devices.";

static int GetDevice(int device, int type)
{
    if (device >= (int)g_mcu_list.size() || type < 0 || type >= 1) {
        return -1;
    }
    if (device < 0) {
        return (int)g_mcu_list.size();
    }
    if (type == 0) {
        return g_mcu_list[device]->m_midi_in_dev;
    }
    if (type == 1) {
        return g_mcu_list[device]->m_midi_out_dev;
    }
    return -1;
}

static const char* defstring_GetMIDIMessage =
    "int\0int,int,int*,int*,int*,int*,char*,int\0"
    "device,msgIdx,statusOut,data1Out,data2Out,frame_offsetOut,msgOutOptional,"
    "msgOutOptional_sz\0"
    "Gets MIDI message from input buffer/queue. "
    "Gets (pops/pulls) indexed message (status, data1, data2 and frame_offset) "
    "from queue and retval is total size/length left in queue. "
    "E.g. continuously read all indiviual messages with deferred script. "
    "Frame offset resolution is 1/1024000 seconds, not audio samples. "
    "Long messages are returned as optional strings of byte characters. "
    "msgIdx -1 returns size (length) of buffer. "
    "Read also non-MCU devices by creating MCULive device with their input. ";
static int GetMIDIMessage(
    int device,
    int msgIdx,
    int* statusOut,
    int* data1Out,
    int* data2Out,
    int* frame_offsetOut,
    char* msgOutOptional,
    int msgOutOptional_sz)
{
    if (device >= (int)g_mcu_list.size() || device < 0) {
        return -1;
    }
    if (g_mcu_list[device]->midiBuffer.empty()) {
        return 0;
    }

    if (msgIdx > (int)g_mcu_list[device]->midiBuffer.size()) {
        return -1;
    }

    if (msgIdx == -1) {
        return (int)g_mcu_list[device]->midiBuffer.size();
    }

    auto n = g_mcu_list[device]->midiBuffer[msgIdx].size;
    if (n > 3 && n < msgOutOptional_sz) {
        memcpy(
            msgOutOptional,
            g_mcu_list[device]->midiBuffer[msgIdx].midi_message,
            n);
    }
    else if (n < 3) {
        return -1;
    }

    *statusOut = g_mcu_list[device]->midiBuffer[msgIdx].midi_message[0];
    *data1Out = g_mcu_list[device]->midiBuffer[msgIdx].midi_message[1];
    *data2Out = g_mcu_list[device]->midiBuffer[msgIdx].midi_message[2];
    *frame_offsetOut = g_mcu_list[device]->midiBuffer[msgIdx].frame_offset;

    g_mcu_list[device]->midiBuffer.erase(
        g_mcu_list[device]->midiBuffer.begin() + msgIdx);
    return (int)g_mcu_list[device]->midiBuffer.size();
}

static const char* defstring_SendMIDIMessage =
    "int\0int,int,int,int,const char*,int\0"
    "device,status,data1,data2,msgInOptional,msgInOptional_sz\0"
    "Sends MIDI message to device. If string is provided, individual bytes are "
    "not sent. Returns number of sent bytes.";
static int SendMIDIMessage(
    int device,
    int status,
    int data1,
    int data2,
    const char* msgInOptional,
    int msgInOptional_sz)
{
    if (device >= (int)g_mcu_list.size()) {
        return -1;
    }
    if (!(g_mcu_list[device]->m_midiout)) {
        return 0;
    }
    auto output = g_mcu_list[device]->m_midi_out_dev;
    int res = 0;
    if (msgInOptional) {
        res = (int)strlen(msgInOptional);
    }
    if (res && msgInOptional_sz != NULL) {
        SendMIDIMessageToHardware(output, msgInOptional, msgInOptional_sz);
    }
    else {
        res = 3;
        g_mcu_list[device]->m_midiout->Send(
            (unsigned char)(status & 7),
            (unsigned char)(data1 & 7),
            (unsigned char)(data2 & 7),
            -1);
    }
    return res;
}

void RegisterAPI()
{
    plugin_register("API_MCULive_SendMIDIMessage", (void*)&SendMIDIMessage);
    plugin_register(
        "APIdef_MCULive_SendMIDIMessage",
        (void*)defstring_SendMIDIMessage);
    plugin_register(
        "APIvararg_MCULive_SendMIDIMessage",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SendMIDIMessage>));

    plugin_register("API_MCULive_GetMIDIMessage", (void*)&GetMIDIMessage);
    plugin_register(
        "APIdef_MCULive_GetMIDIMessage",
        (void*)defstring_GetMIDIMessage);
    plugin_register(
        "APIvararg_MCULive_GetMIDIMessage",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&GetMIDIMessage>));

    plugin_register("API_MCULive_GetDevice", (void*)&GetDevice);
    plugin_register("APIdef_MCULive_GetDevice", (void*)defstring_GetDevice);
    plugin_register(
        "APIvararg_MCULive_GetDevice",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&GetDevice>));

    plugin_register("API_MCULive_SetDisplay", (void*)&SetDisplay);
    plugin_register("APIdef_MCULive_SetDisplay", (void*)defstring_SetDisplay);
    plugin_register(
        "APIvararg_MCULive_SetDisplay",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SetDisplay>));

    plugin_register("API_MCULive_SetOption", (void*)&SetOption);
    plugin_register("APIdef_MCULive_SetOption", (void*)defstring_SetOption);
    plugin_register(
        "APIvararg_MCULive_SetOption",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SetOption>));

    plugin_register("API_MCULive_Reset", (void*)&Reset);
    plugin_register("APIdef_MCULive_Reset", (void*)defstring_Reset);
    plugin_register(
        "APIvararg_MCULive_Reset",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&Reset>));

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

    plugin_register("API_MCULive_SetButtonValue", (void*)&SetButtonValue);
    plugin_register(
        "APIdef_MCULive_SetButtonValue",
        (void*)defstring_SetButtonValue);
    plugin_register(
        "APIvararg_MCULive_SetButtonValue",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SetButtonValue>));

    plugin_register("API_MCULive_GetButtonValue", (void*)&GetButtonValue);
    plugin_register(
        "APIdef_MCULive_GetButtonValue",
        (void*)defstring_GetButtonValue);
    plugin_register(
        "APIvararg_MCULive_GetButtonValue",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&GetButtonValue>));

    plugin_register("API_MCULive_GetFaderValue", (void*)&GetFaderValue);
    plugin_register(
        "APIdef_MCULive_GetFaderValue",
        (void*)defstring_GetFaderValue);
    plugin_register(
        "APIvararg_MCULive_GetFaderValue",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&GetFaderValue>));

    plugin_register("API_MCULive_GetEncoderValue", (void*)&GetEncoderValue);
    plugin_register(
        "APIdef_MCULive_GetEncoderValue",
        (void*)defstring_GetEncoderValue);
    plugin_register(
        "APIvararg_MCULive_GetEncoderValue",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&GetEncoderValue>));

    plugin_register("API_MCULive_SetFaderValue", (void*)&SetFaderValue);
    plugin_register(
        "APIdef_MCULive_SetFaderValue",
        (void*)defstring_SetFaderValue);
    plugin_register(
        "APIvararg_MCULive_SetFaderValue",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SetFaderValue>));

    plugin_register("API_MCULive_SetEncoderValue", (void*)&SetEncoderValue);
    plugin_register(
        "APIdef_MCULive_SetEncoderValue",
        (void*)defstring_SetEncoderValue);
    plugin_register(
        "APIvararg_MCULive_SetEncoderValue",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SetEncoderValue>));

    plugin_register("API_MCULive_SetMeterValue", (void*)&SetMeterValue);
    plugin_register(
        "APIdef_MCULive_SetMeterValue",
        (void*)defstring_SetMeterValue);
    plugin_register(
        "APIvararg_MCULive_SetMeterValue",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SetMeterValue>));

    plugin_register(
        "API_MCULive_SetButtonPassthrough",
        (void*)&SetButtonPassthrough);
    plugin_register(
        "APIdef_MCULive_SetButtonPassthrough",
        (void*)defstring_SetButtonPassthrough);
    plugin_register(
        "APIvararg_MCULive_SetButtonPassthrough",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SetButtonPassthrough>));

    plugin_register("API_MCULive_SetDefault", (void*)&SetDefault);
    plugin_register("APIdef_MCULive_SetDefault", (void*)defstring_SetDefault);
    plugin_register(
        "APIvararg_MCULive_SetDefault",
        reinterpret_cast<void*>(&InvokeReaScriptAPI<&SetDefault>));
    return;
}

} // namespace ReaMCULive