/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <board.h>
#include <eda_item.h>
#include <eda_units.h>
#include <pcb_marker.h>
#include <project/component_class_settings.h>
#include <widgets/report_severity.h>
#include <wx/event.h>
#include <wx/timer.h>

class BOARD_DESIGN_SETTINGS;
class PCB_EDIT_FRAME;
class PROGRESS_REPORTER;


struct LIVE_DRC_VIOLATION
{
    std::unique_ptr<PCB_MARKER> marker;     ///< Not on any board; its DRC item holds no rule or test
    SEVERITY                    severity = RPT_SEVERITY_ERROR;
};

using LIVE_DRC_RESULTS = std::vector<LIVE_DRC_VIOLATION>;


/**
 * Everything a background check needs, copied from the board on the UI thread.
 */
struct LIVE_DRC_SNAPSHOT
{
    std::string                                  boardText;
    wxString                                     boardFileName;
    wxString                                     rulesPath;
    EDA_UNITS                                    units = EDA_UNITS::MM;
    std::shared_ptr<BOARD_DESIGN_SETTINGS>       designSettings;
    std::vector<COMPONENT_CLASS_ASSIGNMENT_DATA> componentClassAssignments;
    bool                                         generateSheetClasses = false;
};


/**
 * Draws live DRC results with the DRC error and warning colors and visibility.  Never selectable
 * and never part of the board.
 */
class LIVE_DRC_OVERLAY : public EDA_ITEM
{
public:
    LIVE_DRC_OVERLAY();

    void SetResults( std::shared_ptr<const LIVE_DRC_RESULTS> aResults ) { m_results = std::move( aResults ); }

    const BOX2I ViewBBox() const override;

    void ViewDraw( int aLayer, KIGFX::VIEW* aView ) const override;

    std::vector<int> ViewGetLayers() const override;

    bool HitTest( const VECTOR2I& aPoint, int aAccuracy = 0 ) const override { return false; }

#if defined( DEBUG )
    void Show( int x, std::ostream& st ) const override {}
#endif

    wxString GetClass() const override { return wxT( "LIVE_DRC_OVERLAY" ); }

private:
    std::shared_ptr<const LIVE_DRC_RESULTS> m_results;
};


/**
 * Continuous DRC of the clearance-type checks.
 *
 * Board changes only schedule work.  Once editing has been idle for a while the board is
 * serialized on the UI thread, then parsed and checked on a worker: DRC builds caches and bumps
 * board timestamps, which it must never do to the board being edited.  Results are published
 * only between interactive operations, and only if the board has not changed since the snapshot.
 */
class LIVE_DRC : public wxEvtHandler, public BOARD_LISTENER
{
public:
    explicit LIVE_DRC( PCB_EDIT_FRAME* aFrame );
    ~LIVE_DRC() override;

    /// The DRC providers run in the background, which report the violation types shown.
    static const std::set<wxString>& Providers();

    static bool IsLiveViolation( int aErrorCode );

    /// Copy what a check needs from \a aBoard.  UI thread only.
    static LIVE_DRC_SNAPSHOT Snapshot( BOARD& aBoard, EDA_UNITS aUnits );

    /**
     * Check a snapshot.  Safe on any thread.
     *
     * @param aReporter may be null; when it reports cancellation the check stops early.
     * @throw IO_ERROR or PARSE_ERROR when the snapshot or the rules cannot be read.
     */
    static LIVE_DRC_RESULTS Check( LIVE_DRC_SNAPSHOT aSnapshot, PROGRESS_REPORTER* aReporter );

    std::shared_ptr<const LIVE_DRC_RESULTS> GetResults() const { return m_results; }

    /// Status of the checker for display, such as "Checking..." or an error.
    const wxString& GetStatus() const { return m_status; }

    /// Called on the UI thread whenever the results or the status change.
    void SetChangedHandler( std::function<void()> aHandler ) { m_changedHandler = std::move( aHandler ); }

    void OnBoardItemAdded( BOARD& aBoard, BOARD_ITEM* aItem ) override;
    void OnBoardItemsAdded( BOARD& aBoard, std::vector<BOARD_ITEM*>& aItems ) override;
    void OnBoardItemRemoved( BOARD& aBoard, BOARD_ITEM* aItem ) override;
    void OnBoardItemsRemoved( BOARD& aBoard, std::vector<BOARD_ITEM*>& aItems ) override;
    void OnBoardItemChanged( BOARD& aBoard, BOARD_ITEM* aItem ) override;
    void OnBoardItemsChanged( BOARD& aBoard, std::vector<BOARD_ITEM*>& aItems ) override;
    void OnBoardNetSettingsChanged( BOARD& aBoard ) override;
    void OnBoardCompositeUpdate( BOARD& aBoard, std::vector<BOARD_ITEM*>& aAdded,
                                 std::vector<BOARD_ITEM*>& aRemoved,
                                 std::vector<BOARD_ITEM*>& aChanged ) override;

private:
    struct RUN_RESULT;

    void attach( BOARD* aBoard );
    void schedule();
    bool editorBusy();
    void startRun();
    void publish( std::shared_ptr<const LIVE_DRC_RESULTS> aResults );
    void setStatus( const wxString& aStatus );
    void cancelRun();

    void onTimer( wxTimerEvent& aEvent );
    void onRunFinished( wxThreadEvent& aEvent );
    void onBoardChanging( wxCommandEvent& aEvent );
    void onBoardChanged( wxCommandEvent& aEvent );

    static bool hasBoardGeometry( const std::vector<BOARD_ITEM*>& aItems );

private:
    PCB_EDIT_FRAME*                         m_frame;
    BOARD*                                  m_board = nullptr;
    std::unique_ptr<LIVE_DRC_OVERLAY>       m_overlay;
    std::shared_ptr<const LIVE_DRC_RESULTS> m_results;
    std::shared_ptr<RUN_RESULT>             m_pendingResult;   ///< Finished, waiting for an idle editor
    wxTimer                                 m_timer;
    std::jthread                            m_worker;
    std::shared_ptr<std::atomic_bool>       m_cancel;
    unsigned long long                      m_generation = 0;  ///< Bumped by every board change
    std::chrono::steady_clock::time_point   m_idleDeadline;
    wxString                                m_rulesPath;
    long long                               m_rulesTimestamp = 0;
    wxString                                m_status;
    std::function<void()>                   m_changedHandler;
    bool                                    m_dirty = false;
    bool                                    m_running = false;
};
