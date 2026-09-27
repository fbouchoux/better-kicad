/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#pragma once

#include <atomic>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>

#include <board.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/timer.h>

class NET_SETTINGS;
class PCB_EDIT_FRAME;
class PCB_MARKER;
class wxStaticText;


/**
 * Dockable, continuously updated DRC results.
 *
 * A board listener only schedules work.  The live board is serialized by the debounce timer on
 * the UI thread, then parsed and checked on a worker.  This is deliberately a snapshot boundary:
 * DRC providers may build caches and modify board timestamps and must never do that to the board
 * being edited.
 */
class PANEL_ONLINE_DRC : public wxPanel, public BOARD_LISTENER
{
public:
    explicit PANEL_ONLINE_DRC( PCB_EDIT_FRAME* aFrame );
    ~PANEL_ONLINE_DRC() override;

    void Suspend();
    void Resume();
    void Shutdown();

    void OnBoardItemAdded( BOARD& aBoard, BOARD_ITEM* aItem ) override;
    void OnBoardItemsAdded( BOARD& aBoard, std::vector<BOARD_ITEM*>& aItems ) override;
    void OnBoardItemRemoved( BOARD& aBoard, BOARD_ITEM* aItem ) override;
    void OnBoardItemsRemoved( BOARD& aBoard, std::vector<BOARD_ITEM*>& aItems ) override;
    void OnBoardNetSettingsChanged( BOARD& aBoard ) override;
    void OnBoardItemChanged( BOARD& aBoard, BOARD_ITEM* aItem ) override;
    void OnBoardItemsChanged( BOARD& aBoard, std::vector<BOARD_ITEM*>& aItems ) override;
    void OnBoardCompositeUpdate( BOARD& aBoard, std::vector<BOARD_ITEM*>& aAdded,
                                 std::vector<BOARD_ITEM*>& aRemoved,
                                 std::vector<BOARD_ITEM*>& aChanged ) override;

private:
    struct SNAPSHOT;
    struct RESULT;

    void attachToBoard( BOARD* aBoard );
    void schedule();
    void startRun();
    void runSnapshot( SNAPSHOT aSnapshot, std::shared_ptr<std::atomic_bool> aCancel );
    void applyResult( const std::shared_ptr<RESULT>& aResult );
    void refreshList();
    void addTrackHighlights( PCB_MARKER& aMarker );

    void onTimer( wxTimerEvent& aEvent );
    void onBoardChanging( wxCommandEvent& aEvent );
    void onBoardChanged( wxCommandEvent& aEvent );
    void onRunFinished( wxThreadEvent& aEvent );
    void onItemActivated( wxListEvent& aEvent );
    void onCanvasMotion( wxMouseEvent& aEvent );

    static bool containsBoardGeometry( const std::vector<BOARD_ITEM*>& aItems );

private:
    PCB_EDIT_FRAME*                    m_frame;
    BOARD*                             m_board;
    wxStaticText*                      m_status;
    wxListCtrl*                        m_list;
    std::vector<KIID>                  m_listMarkerIds;
    wxTimer                            m_debounceTimer;
    std::jthread                       m_worker;
    std::shared_ptr<std::atomic_bool>  m_cancel;
    unsigned long long                 m_generation;
    wxString                           m_lastRulesPath;
    std::time_t                        m_lastRulesTimestamp;
    bool                               m_dirty;
    bool                               m_running;
    bool                               m_suspended;
    bool                               m_applyingResults;
    bool                               m_shutdown;
    wxString                           m_canvasTooltip;
};
