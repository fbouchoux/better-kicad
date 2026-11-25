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

#include <drc/live_drc.h>

#include <algorithm>
#include <cmath>

#include <board_design_settings.h>
#include <component_classes/component_class_manager.h>
#include <drc/drc_engine.h>
#include <drc/drc_exclusion.h>
#include <drc/drc_item.h>
#include <drc/drc_rule.h>
#include <gal/graphics_abstraction_layer.h>
#include <gal/painter.h>
#include <geometry/shape_line_chain.h>
#include <ki_exception.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_edit_frame.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <pcb_shape.h>
#include <progress_reporter.h>
#include <project.h>
#include <project/net_settings.h>
#include <project/project_file.h>
#include <richio.h>
#include <tool/tool_manager.h>
#include <tools/drc_tool.h>
#include <view/view.h>
#include <wx/filename.h>


wxDEFINE_EVENT( EVT_LIVE_DRC_FINISHED, wxThreadEvent );


namespace
{
constexpr int IDLE_DELAY_MS = 1500;
constexpr int BUSY_POLL_MS = 200;
constexpr int RULES_POLL_MS = 1000;


long long rulesTimestamp( const wxString& aPath )
{
    wxFileName rules( aPath );
    return rules.FileExists() ? static_cast<long long>( rules.GetModificationTime().GetTicks() ) : 0;
}


/// Only reports cancellation, which DRC polls between and inside its tests.
class CANCEL_REPORTER : public PROGRESS_REPORTER
{
public:
    explicit CANCEL_REPORTER( std::shared_ptr<std::atomic_bool> aCancel ) : m_cancel( std::move( aCancel ) ) {}

    void SetNumPhases( int ) override {}
    void AddPhases( int ) override {}
    void BeginPhase( int ) override {}
    void AdvancePhase() override {}
    void AdvancePhase( const wxString& ) override {}
    void Report( const wxString& ) override {}
    void SetCurrentProgress( double ) override {}
    void SetMaxProgress( int ) override {}
    void AdvanceProgress() override {}
    bool KeepRefreshing( bool ) override { return !IsCancelled(); }
    void SetTitle( const wxString& ) override {}
    bool IsCancelled() const override { return m_cancel->load( std::memory_order_relaxed ); }

private:
    std::shared_ptr<std::atomic_bool> m_cancel;
};
} // namespace


struct LIVE_DRC::RUN_RESULT
{
    unsigned long long                      generation = 0;
    std::shared_ptr<const LIVE_DRC_RESULTS> results;
    wxString                                error;
    bool                                    cancelled = false;
};


LIVE_DRC_OVERLAY::LIVE_DRC_OVERLAY() :
        EDA_ITEM( NOT_USED )
{
}


const BOX2I LIVE_DRC_OVERLAY::ViewBBox() const
{
    // Markers scale with the zoom, so their extent is only known while drawing
    BOX2I bbox;
    bbox.SetMaximum();
    return bbox;
}


std::vector<int> LIVE_DRC_OVERLAY::ViewGetLayers() const
{
    return { LAYER_DRC_ERROR, LAYER_DRC_WARNING };
}


void LIVE_DRC_OVERLAY::ViewDraw( int aLayer, KIGFX::VIEW* aView ) const
{
    if( !m_results || m_results->empty() )
        return;

    const SEVERITY severity = aLayer == LAYER_DRC_WARNING ? RPT_SEVERITY_WARNING : RPT_SEVERITY_ERROR;

    KIGFX::GAL*             gal = aView->GetGAL();
    KIGFX::RENDER_SETTINGS* settings = aView->GetPainter()->GetSettings();
    const KIGFX::COLOR4D    color = settings->GetColor( nullptr, aLayer );
    const double            zoom = 1.0 / std::sqrt( gal->GetZoomFactor() );

    for( const LIVE_DRC_VIOLATION& violation : *m_results )
    {
        if( violation.severity != severity )
            continue;

        const PCB_MARKER& marker = *violation.marker;
        marker.SetZoom( zoom );

        // The measured path, so overlapping violations stay distinguishable
        std::vector<PCB_SHAPE> legend;

        if( !marker.GetPath().empty() )
            legend = marker.GetErrorLegendShapes();

        for( const PCB_SHAPE& shape : legend )
        {
            const bool hairline = shape.GetStroke().GetWidth() == 1.0;

            gal->SetIsFill( !hairline );
            gal->SetIsStroke( hairline );
            gal->SetStrokeColor( color );
            gal->SetFillColor( color.WithAlpha( 0.35 ) );
            gal->SetLineWidth( (float) marker.MarkerScale() / 3.0f );

            if( shape.GetShape() == SHAPE_T::SEGMENT )
            {
                if( hairline )
                    gal->DrawLine( shape.GetStart(), shape.GetEnd() );
                else
                    gal->DrawSegment( shape.GetStart(), shape.GetEnd(), shape.GetWidth() );
            }
            else if( shape.GetShape() == SHAPE_T::ARC )
            {
                EDA_ANGLE startAngle, endAngle;
                shape.CalcArcAngles( startAngle, endAngle );

                if( hairline )
                {
                    gal->DrawArc( shape.GetCenter(), shape.GetRadius(), startAngle, shape.GetArcAngle() );
                }
                else
                {
                    gal->DrawArcSegment( shape.GetCenter(), shape.GetRadius(), startAngle, shape.GetArcAngle(),
                                         shape.GetWidth(), ARC_HIGH_DEF );
                }
            }
        }

        SHAPE_LINE_CHAIN polygon;
        marker.ShapeToPolygon( polygon );

        gal->Save();
        gal->Translate( marker.GetPosition() );
        gal->SetIsFill( true );
        gal->SetIsStroke( false );
        gal->SetFillColor( color );
        gal->DrawPolygon( polygon );
        gal->Restore();
    }
}


LIVE_DRC::LIVE_DRC( PCB_EDIT_FRAME* aFrame ) :
        m_frame( aFrame ),
        m_overlay( std::make_unique<LIVE_DRC_OVERLAY>() )
{
    m_timer.SetOwner( this );
    Bind( wxEVT_TIMER, &LIVE_DRC::onTimer, this, m_timer.GetId() );
    Bind( EVT_LIVE_DRC_FINISHED, &LIVE_DRC::onRunFinished, this );

    m_frame->Bind( EDA_EVT_BOARD_CHANGING, &LIVE_DRC::onBoardChanging, this );
    m_frame->Bind( EDA_EVT_BOARD_CHANGED, &LIVE_DRC::onBoardChanged, this );

    attach( m_frame->GetBoard() );
    schedule();
}


LIVE_DRC::~LIVE_DRC()
{
    m_timer.Stop();
    cancelRun();

    if( m_worker.joinable() )
        m_worker.join();

    attach( nullptr );

    m_frame->Unbind( EDA_EVT_BOARD_CHANGING, &LIVE_DRC::onBoardChanging, this );
    m_frame->Unbind( EDA_EVT_BOARD_CHANGED, &LIVE_DRC::onBoardChanged, this );
    DeletePendingEvents();
}


const std::set<wxString>& LIVE_DRC::Providers()
{
    static const std::set<wxString> providers = { wxT( "clearance" ), wxT( "creepage" ), wxT( "edge_clearance" ),
                                                  wxT( "hole_to_hole_clearance" ), wxT( "physical_clearance" ),
                                                  wxT( "footprint checks" ) };
    return providers;
}


bool LIVE_DRC::IsLiveViolation( int aErrorCode )
{
    switch( aErrorCode )
    {
    case DRCE_SHORTING_ITEMS:
    case DRCE_CLEARANCE:
    case DRCE_CREEPAGE:
    case DRCE_TRACKS_CROSSING:
    case DRCE_EDGE_CLEARANCE:
    case DRCE_ZONES_INTERSECT:
    case DRCE_DRILLED_HOLES_TOO_CLOSE:
    case DRCE_DRILLED_HOLES_COLOCATED:
    case DRCE_HOLE_CLEARANCE:
        return true;

    default:
        return false;
    }
}


LIVE_DRC_SNAPSHOT LIVE_DRC::Snapshot( BOARD& aBoard, EDA_UNITS aUnits )
{
    LIVE_DRC_SNAPSHOT snapshot;

    PCB_IO_KICAD_SEXPR io;
    STRING_FORMATTER   formatter;
    io.FormatBoardToFormatter( &formatter, &aBoard );

    snapshot.boardText = std::move( formatter.MutableString() );
    snapshot.boardFileName = aBoard.GetFileName();
    snapshot.rulesPath = aBoard.GetDesignRulesPath();
    snapshot.units = aUnits;

    // Most design settings live in the project file rather than the board
    snapshot.designSettings = std::make_shared<BOARD_DESIGN_SETTINGS>( nullptr, "" );
    *snapshot.designSettings = aBoard.GetDesignSettings();
    snapshot.designSettings->m_DRCEngine.reset();

    auto netSettings = std::make_shared<NET_SETTINGS>( nullptr, "" );

    if( aBoard.GetDesignSettings().m_NetSettings )
        netSettings->CopyFrom( *aBoard.GetDesignSettings().m_NetSettings );

    snapshot.designSettings->m_NetSettings = std::move( netSettings );

    if( PROJECT* project = aBoard.GetProject() )
    {
        if( std::shared_ptr<COMPONENT_CLASS_SETTINGS> classes = project->GetProjectFile().ComponentClassSettings() )
        {
            snapshot.componentClassAssignments = classes->GetComponentClassAssignments();
            snapshot.generateSheetClasses = classes->GetEnableSheetComponentClasses();
        }
    }

    return snapshot;
}


LIVE_DRC_RESULTS LIVE_DRC::Check( LIVE_DRC_SNAPSHOT aSnapshot, PROGRESS_REPORTER* aReporter )
{
    LIVE_DRC_RESULTS results;

    auto cancelled =
            [&]()
            {
                return aReporter && aReporter->IsCancelled();
            };

    std::unique_ptr<BOARD> board = std::make_unique<BOARD>();
    board->SetFileName( aSnapshot.boardFileName );

    {
        PCB_IO_KICAD_SEXPR io;
        STRING_LINE_READER reader( aSnapshot.boardText, wxT( "live DRC snapshot" ) );
        io.DoLoad( reader, *board, true, nullptr, nullptr, 0 );
    }

    if( cancelled() )
        return results;

    BOARD_DESIGN_SETTINGS& settings = board->GetDesignSettings();
    settings = *aSnapshot.designSettings;

    board->BuildListOfNets();
    board->SynchronizeNetsAndNetClasses( false );
    board->BuildConnectivity();

    COMPONENT_CLASS_MANAGER& classes = board->GetComponentClassManager();
    classes.SyncDynamicComponentClassAssignments( aSnapshot.componentClassAssignments,
                                                  aSnapshot.generateSheetClasses, {} );
    classes.RebuildRequiredCaches();

    if( cancelled() )
        return results;

    auto engine = std::make_shared<DRC_ENGINE>( board.get(), &settings );
    settings.m_DRCEngine = engine;
    engine->SetProgressReporter( aReporter );
    engine->InitEngine( wxFileName( aSnapshot.rulesPath ) );

    engine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& aPathGenerator )
            {
                if( !IsLiveViolation( aItem->GetErrorCode() ) )
                    return;

                SEVERITY severity = settings.GetSeverity( aItem->GetErrorCode() );

                if( DRC_RULE* rule = aItem->GetViolatingRule(); rule && rule->m_Severity != RPT_SEVERITY_UNDEFINED )
                    severity = rule->m_Severity;

                if( severity != RPT_SEVERITY_ERROR && severity != RPT_SEVERITY_WARNING )
                    return;

                LIVE_DRC_VIOLATION violation;
                violation.marker = std::make_unique<PCB_MARKER>( aItem, aPos, aLayer );
                violation.severity = severity;
                aPathGenerator( violation.marker.get() );

                if( settings.m_DrcExclusions.contains( DRC_EXCLUSION::FromMarker( *violation.marker ) ) )
                    return;

                results.push_back( std::move( violation ) );
            } );

    engine->RunTests( aSnapshot.units, false, false, nullptr, Providers() );
    engine->ClearViolationHandler();
    engine->SetProgressReporter( nullptr );

    // The rules and tests these point at go away with the engine
    for( LIVE_DRC_VIOLATION& violation : results )
    {
        DRC_ITEM* item = static_cast<DRC_ITEM*>( violation.marker->GetRCItem().get() );
        item->SetViolatingRule( nullptr );
        item->SetViolatingTest( nullptr );
    }

    settings.m_DRCEngine.reset();
    engine.reset();

    if( cancelled() )
        results.clear();

    return results;
}


void LIVE_DRC::attach( BOARD* aBoard )
{
    if( m_board == aBoard )
        return;

    if( m_board )
        m_board->RemoveListener( this );

    m_board = aBoard;

    if( m_board )
        m_board->AddListener( this );
}


void LIVE_DRC::setStatus( const wxString& aStatus )
{
    if( aStatus == m_status )
        return;

    m_status = aStatus;

    if( m_changedHandler )
        m_changedHandler();
}


void LIVE_DRC::cancelRun()
{
    if( m_cancel )
        m_cancel->store( true, std::memory_order_relaxed );
}


void LIVE_DRC::schedule()
{
    if( !m_board )
        return;

    m_dirty = true;
    ++m_generation;
    m_idleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( IDLE_DELAY_MS );
    m_pendingResult.reset();
    cancelRun();

    setStatus( _( "Check pending..." ) );
    m_timer.StartOnce( IDLE_DELAY_MS );
}


bool LIVE_DRC::editorBusy()
{
    // An editing tool can stay on the stack while selection is the current tool, and a modal
    // dialog disables the frame
    if( !m_frame->ToolStackIsEmpty() || !m_frame->IsEnabled() || m_frame->IsInteractiveOperationInProgress() )
        return true;

    DRC_TOOL* drcTool = m_frame->GetToolManager()->GetTool<DRC_TOOL>();
    return drcTool && drcTool->IsDRCRunning();
}


void LIVE_DRC::onTimer( wxTimerEvent& )
{
    if( !m_board )
        return;

    if( editorBusy() )
    {
        m_idleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( IDLE_DELAY_MS );

        // A tool may be editing what the worker reads, so check again once it is done
        if( m_running && m_cancel && !m_cancel->exchange( true, std::memory_order_relaxed ) )
        {
            m_dirty = true;
            ++m_generation;
        }

        m_timer.StartOnce( BUSY_POLL_MS );
        return;
    }

    if( m_running || std::chrono::steady_clock::now() < m_idleDeadline )
    {
        m_timer.StartOnce( BUSY_POLL_MS );
        return;
    }

    if( m_pendingResult )
    {
        std::shared_ptr<RUN_RESULT> result = std::move( m_pendingResult );

        if( result->generation == m_generation )
            publish( result->results );
    }

    if( m_dirty )
        startRun();
    else if( m_board->GetDesignRulesPath() != m_rulesPath
             || rulesTimestamp( m_board->GetDesignRulesPath() ) != m_rulesTimestamp )
        schedule();
    else
        m_timer.StartOnce( RULES_POLL_MS );
}


void LIVE_DRC::startRun()
{
    try
    {
        LIVE_DRC_SNAPSHOT snapshot = Snapshot( *m_board, m_frame->GetUserUnits() );

        m_rulesPath = snapshot.rulesPath;
        m_rulesTimestamp = rulesTimestamp( m_rulesPath );

        if( m_worker.joinable() )
            m_worker.join();

        m_dirty = false;
        m_running = true;
        m_cancel = std::make_shared<std::atomic_bool>( false );

        m_worker = std::jthread(
                [this, snapshot = std::move( snapshot ), cancel = m_cancel, generation = m_generation]() mutable
                {
                    auto            result = std::make_shared<RUN_RESULT>();
                    CANCEL_REPORTER reporter( cancel );

                    result->generation = generation;

                    try
                    {
                        result->results = std::make_shared<LIVE_DRC_RESULTS>( Check( std::move( snapshot ),
                                                                                     &reporter ) );
                    }
                    catch( const IO_ERROR& error )
                    {
                        result->error = error.What();
                    }
                    catch( const std::exception& error )
                    {
                        result->error = wxString::FromUTF8( error.what() );
                    }
                    catch( ... )
                    {
                        result->error = wxS( "unknown error" );
                    }

                    result->cancelled = cancel->load( std::memory_order_relaxed );

                    wxThreadEvent* event = new wxThreadEvent( EVT_LIVE_DRC_FINISHED );
                    event->SetPayload( result );
                    wxQueueEvent( this, event );
                } );

        setStatus( _( "Checking..." ) );
    }
    catch( const IO_ERROR& error )
    {
        setStatus( wxString::Format( _( "Live DRC could not copy the board: %s" ), error.What() ) );
    }
    catch( const std::exception& error )
    {
        setStatus( wxString::Format( _( "Live DRC could not copy the board: %s" ),
                                     wxString::FromUTF8( error.what() ) ) );
    }

    m_timer.StartOnce( m_running ? BUSY_POLL_MS : RULES_POLL_MS );
}


void LIVE_DRC::onRunFinished( wxThreadEvent& aEvent )
{
    std::shared_ptr<RUN_RESULT> result = aEvent.GetPayload<std::shared_ptr<RUN_RESULT>>();

    if( m_worker.joinable() )
        m_worker.join();

    m_running = false;
    m_cancel.reset();

    if( result->cancelled || result->generation != m_generation )
        return;

    if( !result->error.IsEmpty() )
    {
        setStatus( wxString::Format( _( "Live DRC failed: %s" ), result->error ) );
        return;
    }

    // Published from the timer, which waits for the editor to be idle
    m_pendingResult = std::move( result );
    m_timer.StartOnce( BUSY_POLL_MS );
}


void LIVE_DRC::publish( std::shared_ptr<const LIVE_DRC_RESULTS> aResults )
{
    m_results = std::move( aResults );
    m_overlay->SetResults( m_results );

    KIGFX::VIEW* view = m_frame->GetCanvas()->GetView();

    // Loading a board clears the view, so the overlay is registered again every time
    view->Add( m_overlay.get() );
    view->MarkTargetDirty( KIGFX::TARGET_OVERLAY );
    m_frame->GetCanvas()->Refresh();

    size_t count = m_results ? m_results->size() : 0;
    m_status = wxString::Format( count == 1 ? _( "%zu violation" ) : _( "%zu violations" ), count );

    if( m_changedHandler )
        m_changedHandler();
}


void LIVE_DRC::onBoardChanging( wxCommandEvent& aEvent )
{
    attach( nullptr );

    ++m_generation;
    m_dirty = false;
    m_pendingResult.reset();
    m_timer.Stop();
    cancelRun();

    m_results.reset();
    m_overlay->SetResults( nullptr );
    m_status.clear();

    if( m_changedHandler )
        m_changedHandler();

    aEvent.Skip();
}


void LIVE_DRC::onBoardChanged( wxCommandEvent& aEvent )
{
    attach( m_frame->GetBoard() );
    schedule();
    aEvent.Skip();
}


bool LIVE_DRC::hasBoardGeometry( const std::vector<BOARD_ITEM*>& aItems )
{
    return std::any_of( aItems.begin(), aItems.end(),
                        []( const BOARD_ITEM* aItem )
                        {
                            return aItem && aItem->Type() != PCB_MARKER_T;
                        } );
}


void LIVE_DRC::OnBoardItemAdded( BOARD&, BOARD_ITEM* aItem )
{
    if( aItem && aItem->Type() != PCB_MARKER_T )
        schedule();
}


void LIVE_DRC::OnBoardItemsAdded( BOARD&, std::vector<BOARD_ITEM*>& aItems )
{
    if( hasBoardGeometry( aItems ) )
        schedule();
}


void LIVE_DRC::OnBoardItemRemoved( BOARD&, BOARD_ITEM* aItem )
{
    if( aItem && aItem->Type() != PCB_MARKER_T )
        schedule();
}


void LIVE_DRC::OnBoardItemsRemoved( BOARD&, std::vector<BOARD_ITEM*>& aItems )
{
    if( hasBoardGeometry( aItems ) )
        schedule();
}


void LIVE_DRC::OnBoardItemChanged( BOARD&, BOARD_ITEM* aItem )
{
    if( aItem && aItem->Type() != PCB_MARKER_T )
        schedule();
}


void LIVE_DRC::OnBoardItemsChanged( BOARD&, std::vector<BOARD_ITEM*>& aItems )
{
    if( hasBoardGeometry( aItems ) )
        schedule();
}


void LIVE_DRC::OnBoardNetSettingsChanged( BOARD& )
{
    schedule();
}


void LIVE_DRC::OnBoardCompositeUpdate( BOARD&, std::vector<BOARD_ITEM*>& aAdded, std::vector<BOARD_ITEM*>& aRemoved,
                                       std::vector<BOARD_ITEM*>& aChanged )
{
    if( hasBoardGeometry( aAdded ) || hasBoardGeometry( aRemoved ) || hasBoardGeometry( aChanged ) )
        schedule();
}
