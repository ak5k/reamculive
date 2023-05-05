/*
** reaper_csurf
** Copyright (C) 2006-2008 Cockos Incorporated
** License: LGPL.
*/

#include <string>

#define REAPERAPI_IMPLEMENT
#include "reaper_plugin_functions.h"

#define LOCALIZE_IMPORT_PREFIX "csurf_"

#include "../localize-import.h"
#include "csurf.h"

#include "../../WDL/win32_utf8.c"

#include "../../WDL/setthreadname.h"

namespace ReaMCULive {

void RegisterAPI();
extern reaper_csurf_reg_t csurf_mcu_reg, csurf_mcuex_reg;

REAPER_PLUGIN_HINSTANCE g_hInst; // used for dialogs, if any
HWND g_hwnd;

int* g_config_csurf_rate;
int* g_config_zoommode;

int* g_vu_minvol;
int* g_vu_maxvol;
int* g_config_vudecay;

int __g_projectconfig_timemode2;
int __g_projectconfig_timemode;
int __g_projectconfig_measoffs;
int __g_projectconfig_timeoffs; // double
int __g_projectconfig_show_grid;
int __g_projectconfig_autoxfade;
int __g_projectconfig_metronome_en;

} // namespace ReaMCULive

extern "C" {

REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE hInstance,
    reaper_plugin_info_t* rec)
{
    ReaMCULive::g_hInst = hInstance;

    if (!rec || rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc ||
        REAPERAPI_LoadAPI(rec->GetFunc))
        return 0;

    ReaMCULive::g_hwnd = rec->hwnd_main;
    int errcnt = 0;

    int sztmp;
#define IMPVAR(x, nm)                                      \
    if (!((*(void**)&(x)) = get_config_var(nm, &sztmp)) || \
        sztmp != sizeof(*x))                               \
        errcnt++;
#define IMPVARP(x, nm, type)                              \
    if (!((x) = projectconfig_var_getoffs(nm, &sztmp)) || \
        sztmp != sizeof(type))                            \
        errcnt++;
    IMPVAR(ReaMCULive::g_config_csurf_rate, "csurfrate")
    IMPVAR(ReaMCULive::g_config_zoommode, "zoommode")
    IMPVAR(ReaMCULive::g_vu_minvol, "vuminvol");
    IMPVAR(ReaMCULive::g_vu_maxvol, "vumaxvol");
    IMPVAR(ReaMCULive::g_config_vudecay, "vudecay");

    IMPVARP(ReaMCULive::__g_projectconfig_timemode, "projtimemode", int)
    IMPVARP(ReaMCULive::__g_projectconfig_timemode2, "projtimemode2", int)
    IMPVARP(ReaMCULive::__g_projectconfig_timeoffs, "projtimeoffs", double);
    IMPVARP(ReaMCULive::__g_projectconfig_measoffs, "projmeasoffs", int);
    IMPVARP(ReaMCULive::__g_projectconfig_show_grid, "projshowgrid", int);
    IMPVARP(ReaMCULive::__g_projectconfig_autoxfade, "autoxfade", int);
    IMPVARP(ReaMCULive::__g_projectconfig_metronome_en, "projmetroen", int);

    if (errcnt)
        return 0;

    auto ver = std::stod(GetAppVersion());

    if (ver < 6.76) {
        ShowConsoleMsg("ReaMCULive: REAPER 6.76 or later required.");
        return 0;
    }

    // Plugin_Register = rec->Register;

    rec->Register("csurf", &ReaMCULive::csurf_mcu_reg);
    rec->Register("csurf", &ReaMCULive::csurf_mcuex_reg);

    ReaMCULive::RegisterAPI();

    IMPORT_LOCALIZE_RPLUG(rec)
    return 1;
}
};

#ifndef _WIN32 // MAC resources
#include "../../WDL/swell/swell-dlggen.h"
#include "res.rc_mac_dlg"
#undef BEGIN
#undef END
#include "../../WDL/swell/swell-menugen.h"
#include "res.rc_mac_menu"
#endif

namespace ReaMCULive {

#ifndef _WIN32 // let OS X use this threading step

#include "../../WDL/mutex.h"
#include "../../WDL/ptrlist.h"

class threadedMIDIOutput : public midi_Output {
  public:
    threadedMIDIOutput(midi_Output* out)
    {
        m_output = out;
        m_quit = 0;
        unsigned id;
        m_hThread = (HANDLE)_beginthreadex(NULL, 0, threadProc, this, 0, &id);
    }

    virtual ~threadedMIDIOutput()
    {
        if (m_hThread) {
            m_quit = 1;
            WaitForSingleObject(m_hThread, INFINITE);
            CloseHandle(m_hThread);
            m_hThread = 0;
            Sleep(30);
        }

        if (m_output)
            m_output->Destroy();
        m_empty.Empty(true);
        m_full.Empty(true);
    }

    virtual void Destroy()
    {
        HANDLE thread = m_hThread;
        if (!thread) {
            delete this;
        }
        else {
            m_hThread = NULL;
            m_quit = 2;

            // thread will delete self
            WaitForSingleObject(thread, 100);
            CloseHandle(thread);
        }
    }

    virtual void SendMsg(
        MIDI_event_t* msg,
        int frame_offset) // frame_offset can be <0 for "instant" if supported
    {
        if (!msg)
            return;

        WDL_HeapBuf* b = NULL;
        if (m_empty.GetSize()) {
            m_mutex.Enter();
            b = m_empty.Get(m_empty.GetSize() - 1);
            m_empty.Delete(m_empty.GetSize() - 1);
            m_mutex.Leave();
        }
        if (!b && m_empty.GetSize() + m_full.GetSize() < 500)
            b = new WDL_HeapBuf(256);

        if (b) {
            int sz = msg->size;
            if (sz < 3)
                sz = 3;
            int len = msg->midi_message + sz - (unsigned char*)msg;
            memcpy(b->Resize(len, false), msg, len);
            m_mutex.Enter();
            m_full.Add(b);
            m_mutex.Leave();
        }
    }

    virtual void Send(
        unsigned char status,
        unsigned char d1,
        unsigned char d2,
        int frame_offset) // frame_offset can be <0 for "instant" if supported
    {
        MIDI_event_t evt = {0, 3, status, d1, d2};
        SendMsg(&evt, frame_offset);
    }

    ///////////

    static unsigned WINAPI threadProc(LPVOID p)
    {
        WDL_SetThreadName("reaper/cs_midio");
        WDL_HeapBuf* lastbuf = NULL;
        threadedMIDIOutput* _this = (threadedMIDIOutput*)p;
        unsigned int scnt = 0;
        for (;;) {
            if (_this->m_full.GetSize() || lastbuf) {
                _this->m_mutex.Enter();
                if (lastbuf)
                    _this->m_empty.Add(lastbuf);
                lastbuf = _this->m_full.Get(0);
                _this->m_full.Delete(0);
                _this->m_mutex.Leave();

                if (lastbuf)
                    _this->m_output->SendMsg((MIDI_event_t*)lastbuf->Get(), -1);
                scnt = 0;
            }
            else {
                Sleep(1);
                if (_this->m_quit && scnt++ > 3)
                    break; // only quit once all messages have been sent
            }
        }
        delete lastbuf;
        if (_this->m_quit == 2)
            delete _this;
        return 0;
    }

    WDL_Mutex m_mutex;
    WDL_PtrList<WDL_HeapBuf> m_full, m_empty;

    HANDLE m_hThread;
    int m_quit; // set to 1 to finish, 2 to finish+delete self
    midi_Output* m_output;
};

midi_Output* CreateThreadedMIDIOutput(midi_Output* output)
{
    if (!output)
        return output;
    return new threadedMIDIOutput(output);
}

#else

// windows doesnt need it since we have threaded midi outputs now
midi_Output* CreateThreadedMIDIOutput(midi_Output* output)
{
    return output;
}

#endif
} // namespace ReaMCULive