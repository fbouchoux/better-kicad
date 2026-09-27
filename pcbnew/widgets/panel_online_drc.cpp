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

#include "panel_online_drc.h"

#include <algorithm>
#include <exception>

#include <board_commit.h>
#include <board_design_settings.h>
#include <component_classes/component_class_manager.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <drc/drc_rule.h>
#include <dialog_drc.h>
#include <ki_exception.h>
#include <math/util.h>
#include <pcb_edit_frame.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <pcb_marker.h>
#include <pcb_track.h>
#include <progress_reporter.h>
#include <project.h>
#include <project/component_class_settings.h>
#include <project/net_settings.h>
#include <project/project_file.h>
#include <project/tuning_profiles.h>
#include <richio.h>
#include <tool/tool_manager.h>
#include <tools/drc_tool.h>
#include <widgets/wx_data_view_hyperlink_renderer.h>
#include <view/view.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stattext.h>


wxDEFINE_EVENT( EVT_ONLINE_DRC_FINISHED, wxThreadEvent );


namespace
{
constexpr int ONLINE_DRC_DEBOUNCE_MS = 650;


bool isLiveViolation( int aErrorCode )
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


std::time_t rulesTimestamp( const wxString& aPath )
{
    wxFileName rulesFile( aPath );
    return rulesFile.FileExists() ? rulesFile.GetModificationTime().GetTicks() : 0;
}


class ONLINE_DRC_REPORTER : public PROGRESS_REPORTER
{
public:
    explicit ONLINE_DRC_REPORTER( std::shared_ptr<std::atomic_bool> aCancel ) :
            m_cancel( std::move( aCancel ) )
    {
    }

    void SetNumPhases( int ) override {}
    void AddPhases( int ) override {}
    void BeginPhase( int ) override {}
    void AdvancePhase() override {}
    void AdvancePhase( const wxString& ) override {}
    void Report( const wxString& ) override {}
    void SetCurrentProgress( double ) override {}
    void SetMaxProgress( int ) override {}
    void AdvanceProgress() override {}
    void SetTitle( const wxString& ) override {}
    bool KeepRefreshing( bool ) override { return !IsCancelled(); }
    bool IsCancelled() const override { return m_cancel->load( std::memory_order_relaxed ); }

private:
    std::shared_ptr<std::atomic_bool> m_cancel;
};
}


struct PANEL_ONLINE_DRC::SNAPSHOT
{
    std::string                                  boardText;
    wxString                                     boardFileName;
    wxString                                     rulesPath;
    EDA_UNITS                                    units;
    unsigned long long                           generation;
    std::shared_ptr<BOARD_DESIGN_SETTINGS>       designSettings;
    std::shared_ptr<TUNING_PROFILES>             tuningProfiles;
    std::vector<COMPONENT_CLASS_ASSIGNMENT_DATA> componentClassAssignments;
    bool                                         generateSheetClasses = false;
};


struct PANEL_ONLINE_DRC::RESULT
{
    unsigned long long                       generation = 0;
    std::vector<std::unique_ptr<PCB_MARKER>> markers;
    wxString                                 error;
    bool                                     cancelled = false;
};


PANEL_ONLINE_DRC::PANEL_ONLINE_DRC( PCB_EDIT_FRAME* aFrame ) :
        wxPanel( aFrame ),
        m_frame( aFrame ),
        m_board( nullptr ),
        m_status( nullptr ),
        m_list( nullptr ),
        m_generation( 0 ),
        m_lastRulesTimestamp( 0 ),
        m_dirty( false ),
        m_running( false ),
        m_suspended( false ),
        m_applyingResults( false ),
        m_shutdown( false )
{
    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );
    m_status = new wxStaticText( this, wxID_ANY, _( "Waiting for board changes..." ) );
    m_list = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES );
    m_list->AppendColumn( _( "Severity" ), wxLIST_FORMAT_LEFT, FromDIP( 90 ) );
    m_list->AppendColumn( _( "Violation" ), wxLIST_FORMAT_LEFT, FromDIP( 520 ) );

    sizer->Add( m_status, 0, wxEXPAND | wxALL, FromDIP( 5 ) );
    sizer->Add( m_list, 1, wxEXPAND );
    SetSizer( sizer );

    m_debounceTimer.SetOwner( this );
    Bind( wxEVT_TIMER, &PANEL_ONLINE_DRC::onTimer, this, m_debounceTimer.GetId() );
    Bind( EVT_ONLINE_DRC_FINISHED, &PANEL_ONLINE_DRC::onRunFinished, this );
    m_list->Bind( wxEVT_LIST_ITEM_ACTIVATED, &PANEL_ONLINE_DRC::onItemActivated, this );

    m_frame->Bind( EDA_EVT_BOARD_CHANGING, &PANEL_ONLINE_DRC::onBoardChanging, this );
    m_frame->Bind( EDA_EVT_BOARD_CHANGED, &PANEL_ONLINE_DRC::onBoardChanged, this );
    m_frame->GetCanvas()->Bind( wxEVT_MOTION, &PANEL_ONLINE_DRC::onCanvasMotion, this );

    attachToBoard( m_frame->GetBoard() );
    schedule();
}


PANEL_ONLINE_DRC::~PANEL_ONLINE_DRC()
{
    Shutdown();
}


void PANEL_ONLINE_DRC::Shutdown()
{
    if( m_shutdown )
        return;

    m_shutdown = true;
    m_debounceTimer.Stop();

    if( m_cancel )
        m_cancel->store( true, std::memory_order_relaxed );

    if( m_worker.joinable() )
        m_worker.join();

    if( m_board )
        m_board->RemoveListener( this );

    m_frame->Unbind( EDA_EVT_BOARD_CHANGING, &PANEL_ONLINE_DRC::onBoardChanging, this );
    m_frame->Unbind( EDA_EVT_BOARD_CHANGED, &PANEL_ONLINE_DRC::onBoardChanged, this );
    m_frame->GetCanvas()->Unbind( wxEVT_MOTION, &PANEL_ONLINE_DRC::onCanvasMotion, this );
    m_frame->GetCanvas()->SetToolTip( wxString() );
    DeletePendingEvents();
}


void PANEL_ONLINE_DRC::Suspend()
{
    if( m_shutdown )
        return;

    m_suspended = true;
    m_debounceTimer.Stop();
    ++m_generation;

    if( m_cancel )
        m_cancel->store( true, std::memory_order_relaxed );

    m_list->DeleteAllItems();
    m_listMarkerIds.clear();
    m_status->SetLabel( _( "Paused while manual DRC is running..." ) );
}


void PANEL_ONLINE_DRC::Resume()
{
    if( m_shutdown )
        return;

    m_suspended = false;
    schedule();
}


void PANEL_ONLINE_DRC::attachToBoard( BOARD* aBoard )
{
    if( m_board == aBoard )
        return;

    if( m_board )
        m_board->RemoveListener( this );

    m_board = aBoard;

    if( m_board )
        m_board->AddListener( this );
}


void PANEL_ONLINE_DRC::schedule()
{
    if( m_shutdown || m_suspended || !m_board )
        return;

    m_dirty = true;
    ++m_generation;

    if( m_cancel )
        m_cancel->store( true, std::memory_order_relaxed );

    m_status->SetLabel( m_running ? _( "Board changed; restarting check..." )
                                  : _( "Board changed; check pending..." ) );
    m_debounceTimer.StartOnce( ONLINE_DRC_DEBOUNCE_MS );
}


void PANEL_ONLINE_DRC::onTimer( wxTimerEvent& )
{
    if( m_running || m_suspended || !m_board )
        return;

    if( m_dirty )
        startRun();
    else if( m_board->GetDesignRulesPath() != m_lastRulesPath
             || rulesTimestamp( m_board->GetDesignRulesPath() ) != m_lastRulesTimestamp )
        schedule();
    else
        m_debounceTimer.StartOnce( 1000 );
}


void PANEL_ONLINE_DRC::startRun()
{
    if( m_shutdown || m_running || !m_board )
        return;

    try
    {
        PCB_IO_KICAD_SEXPR io;
        STRING_FORMATTER   formatter;
        io.FormatBoardToFormatter( &formatter, m_board, nullptr, false );

        SNAPSHOT snapshot;
        snapshot.boardText = std::move( formatter.MutableString() );
        snapshot.boardFileName = m_board->GetFileName();
        snapshot.rulesPath = m_board->GetDesignRulesPath();
        snapshot.units = m_frame->GetUserUnits();
        snapshot.generation = m_generation;
        snapshot.designSettings = std::make_shared<BOARD_DESIGN_SETTINGS>( nullptr, "" );
        *snapshot.designSettings = m_board->GetDesignSettings();

        std::shared_ptr<NET_SETTINGS> netSettings = std::make_shared<NET_SETTINGS>( nullptr, "" );

        if( m_board->GetDesignSettings().m_NetSettings )
            netSettings->CopyFrom( *m_board->GetDesignSettings().m_NetSettings );

        snapshot.designSettings->m_NetSettings = std::move( netSettings );

        if( PROJECT* project = m_board->GetProject() )
        {
            std::shared_ptr<COMPONENT_CLASS_SETTINGS> componentClasses =
                    project->GetProjectFile().ComponentClassSettings();

            if( componentClasses )
            {
                snapshot.componentClassAssignments = componentClasses->GetComponentClassAssignments();
                snapshot.generateSheetClasses = componentClasses->GetEnableSheetComponentClasses();
            }

            std::shared_ptr<TUNING_PROFILES> tuningProfiles =
                    project->GetProjectFile().TuningProfileParameters();

            if( tuningProfiles )
            {
                snapshot.tuningProfiles = std::make_shared<TUNING_PROFILES>( nullptr, "" );

                for( const TUNING_PROFILE& profile : tuningProfiles->GetTuningProfiles() )
                    snapshot.tuningProfiles->AddTuningProfile( TUNING_PROFILE( profile ) );
            }
        }

        m_lastRulesPath = snapshot.rulesPath;
        m_lastRulesTimestamp = rulesTimestamp( snapshot.rulesPath );
        m_dirty = false;
        m_running = true;
        m_cancel = std::make_shared<std::atomic_bool>( false );
        m_status->SetLabel( _( "Checking board in background..." ) );

        if( m_worker.joinable() )
            m_worker.join();

        std::shared_ptr<std::atomic_bool> cancel = m_cancel;
        m_worker = std::jthread(
                [this, snapshot = std::move( snapshot ), cancel]() mutable
                {
                    runSnapshot( std::move( snapshot ), cancel );
                } );
    }
    catch( const IO_ERROR& error )
    {
        m_status->SetLabel( wxString::Format( _( "Online DRC snapshot failed: %s" ), error.What() ) );
        m_debounceTimer.StartOnce( 1000 );
    }
    catch( const std::exception& error )
    {
        m_status->SetLabel( wxString::Format( _( "Online DRC snapshot failed: %s" ),
                                              wxString::FromUTF8( error.what() ) ) );
        m_debounceTimer.StartOnce( 1000 );
    }
    catch( ... )
    {
        m_status->SetLabel( _( "Online DRC snapshot failed" ) );
        m_debounceTimer.StartOnce( 1000 );
    }
}


void PANEL_ONLINE_DRC::runSnapshot( SNAPSHOT aSnapshot,
                                    std::shared_ptr<std::atomic_bool> aCancel )
{
    std::shared_ptr<RESULT> result = std::make_shared<RESULT>();
    result->generation = aSnapshot.generation;

    try
    {
        PCB_IO_KICAD_SEXPR io;
        STRING_LINE_READER reader( aSnapshot.boardText, wxT( "online DRC snapshot" ) );
        std::unique_ptr<BOARD> board = std::make_unique<BOARD>();
        board->SetFileName( aSnapshot.boardFileName );
        io.DoLoad( reader, *board, true, nullptr, nullptr, 0 );

        BOARD_DESIGN_SETTINGS& settings = board->GetDesignSettings();
        settings = *aSnapshot.designSettings;
        board->BuildConnectivity();
        board->BuildListOfNets();
        board->SynchronizeNetsAndNetClasses( true );
        board->GetComponentClassManager().SyncDynamicComponentClassAssignments(
                aSnapshot.componentClassAssignments, aSnapshot.generateSheetClasses, {} );
        board->GetComponentClassManager().RebuildRequiredCaches();

        std::shared_ptr<DRC_ENGINE> engine = std::make_shared<DRC_ENGINE>( board.get(), &settings );
        settings.m_DRCEngine = engine;
        engine->SetTuningProfiles( std::move( aSnapshot.tuningProfiles ) );
        ONLINE_DRC_REPORTER reporter( aCancel );
        engine->SetProgressReporter( &reporter );
        engine->InitEngine( aSnapshot.rulesPath );

        engine->SetViolationHandler(
                [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                     const std::function<void( PCB_MARKER* )>& aPathGenerator )
                {
                    if( aCancel->load( std::memory_order_relaxed ) )
                        return;

                    if( !isLiveViolation( aItem->GetErrorCode() ) )
                        return;

                    std::unique_ptr<PCB_MARKER> marker =
                            std::make_unique<PCB_MARKER>( aItem, aPos, aLayer );
                    aPathGenerator( marker.get() );
                    marker->SetOnline();

                    SEVERITY severity = settings.GetSeverity( aItem->GetErrorCode() );

                    if( aItem->GetErrorCode() == DRCE_GENERIC_WARNING )
                        severity = RPT_SEVERITY_WARNING;
                    else if( aItem->GetErrorCode() == DRCE_GENERIC_ERROR )
                        severity = RPT_SEVERITY_ERROR;
                    else if( DRC_RULE* rule = aItem->GetViolatingRule();
                             rule && rule->m_Severity != RPT_SEVERITY_UNDEFINED )
                        severity = rule->m_Severity;

                    marker->SetSeverityOverride( severity );
                    result->markers.emplace_back( std::move( marker ) );
                } );

        engine->RunTests( aSnapshot.units, false, false );
        engine->ClearViolationHandler();
        engine->SetProgressReporter( nullptr );

        // DRC_ITEM stores non-owning pointers into its engine.  Preserve the severity above, then
        // detach the pointers before the snapshot engine and its providers are destroyed.
        for( const std::unique_ptr<PCB_MARKER>& marker : result->markers )
        {
            DRC_ITEM* item = static_cast<DRC_ITEM*>( marker->GetRCItem().get() );
            item->SetViolatingRule( nullptr );
            item->SetViolatingTest( nullptr );
        }

        result->cancelled = aCancel->load( std::memory_order_relaxed );
    }
    catch( const IO_ERROR& error )
    {
        result->markers.clear();
        result->error = error.What();
    }
    catch( const std::exception& error )
    {
        result->markers.clear();
        result->error = wxString::FromUTF8( error.what() );
    }
    catch( ... )
    {
        result->markers.clear();
        result->error = _( "Unknown background DRC error" );
    }

    wxThreadEvent* event = new wxThreadEvent( EVT_ONLINE_DRC_FINISHED );
    event->SetPayload( result );
    wxQueueEvent( this, event );
}


void PANEL_ONLINE_DRC::onRunFinished( wxThreadEvent& aEvent )
{
    std::shared_ptr<RESULT> result = aEvent.GetPayload<std::shared_ptr<RESULT>>();

    if( m_worker.joinable() )
        m_worker.join();

    m_running = false;
    m_cancel.reset();

    if( m_shutdown )
        return;

    if( !result->error.IsEmpty() && result->generation == m_generation )
        m_status->SetLabel( wxString::Format( _( "Online DRC failed: %s" ), result->error ) );
    else if( !result->cancelled && result->generation == m_generation && !m_suspended )
        applyResult( result );

    if( m_dirty && !m_suspended )
        m_debounceTimer.StartOnce( 50 );
    else if( !m_suspended )
        m_debounceTimer.StartOnce( 1000 );
}


void PANEL_ONLINE_DRC::applyResult( const std::shared_ptr<RESULT>& aResult )
{
    if( !m_board || aResult->generation != m_generation )
        return;

    m_applyingResults = true;
    m_canvasTooltip.clear();
    m_frame->GetCanvas()->SetToolTip( wxString() );
    BOARD_COMMIT commit( m_frame );

    std::vector<PCB_MARKER*> oldOnlineMarkers;

    for( PCB_MARKER* marker : m_board->Markers() )
    {
        if( marker->IsOnline() )
            oldOnlineMarkers.push_back( marker );
    }

    for( PCB_MARKER* marker : oldOnlineMarkers )
        commit.Remove( marker );

    for( std::unique_ptr<PCB_MARKER>& marker : aResult->markers )
    {
        addTrackHighlights( *marker );
        commit.Add( marker.release() );
    }

    commit.Push( _( "Online DRC" ), SKIP_UNDO | SKIP_SET_DIRTY );
    m_frame->ResolveDRCExclusions( false );
    m_applyingResults = false;

    refreshList();
    m_frame->RefreshCanvas();

    if( DRC_TOOL* tool = m_frame->GetToolManager()->GetTool<DRC_TOOL>();
        tool && tool->GetDRCDialog() )
        tool->GetDRCDialog()->UpdateData();
}


void PANEL_ONLINE_DRC::onCanvasMotion( wxMouseEvent& aEvent )
{
    aEvent.Skip();

    wxString tooltip;
    KIGFX::VIEW* view = m_frame->GetCanvas()->GetView();

    if( m_board && view->IsLayerVisible( LAYER_LIVE_DRC ) )
    {
        VECTOR2D world = view->ToWorld( VECTOR2D( aEvent.GetX(), aEvent.GetY() ) );
        VECTOR2I cursor( KiROUND( world.x ), KiROUND( world.y ) );
        int      accuracy = std::max( 1, KiROUND( view->ToWorld( 8.0 ) ) );

        for( PCB_MARKER* marker : m_board->Markers() )
        {
            if( marker->IsOnline() && marker->HitTest( cursor, accuracy ) )
            {
                tooltip = HYPERLINK_DV_RENDERER::StripMarkup(
                        marker->GetRCItem()->GetErrorMessage( true ) );
                break;
            }
        }
    }

    if( tooltip != m_canvasTooltip )
    {
        m_canvasTooltip = tooltip;
        m_frame->GetCanvas()->SetToolTip( tooltip );
    }
}


void PANEL_ONLINE_DRC::addTrackHighlights( PCB_MARKER& aMarker )
{
    if( aMarker.GetRCItem()->GetErrorCode() != DRCE_SHORTING_ITEMS )
        return;

    std::vector<PCB_SHAPE> highlights;

    for( const KIID& id : aMarker.GetRCItem()->GetIDs() )
    {
        BOARD_ITEM* item = m_board->ResolveItem( id, true );

        if( !item )
            continue;

        if( item->Type() == PCB_TRACE_T )
        {
            PCB_TRACK* track = static_cast<PCB_TRACK*>( item );
            PCB_SHAPE  shape( nullptr, SHAPE_T::SEGMENT );
            shape.SetStart( track->GetStart() );
            shape.SetEnd( track->GetEnd() );
            shape.SetWidth( track->GetWidth() + pcbIUScale.mmToIU( 0.12 ) );
            highlights.push_back( std::move( shape ) );
        }
        else if( item->Type() == PCB_ARC_T )
        {
            PCB_ARC*  track = static_cast<PCB_ARC*>( item );
            PCB_SHAPE shape( nullptr, SHAPE_T::ARC );
            shape.SetArcGeometry( track->GetStart(), track->GetMid(), track->GetEnd() );
            shape.SetWidth( track->GetWidth() + pcbIUScale.mmToIU( 0.12 ) );
            highlights.push_back( std::move( shape ) );
        }
        else if( item->Type() == PCB_VIA_T )
        {
            PCB_VIA*   via = static_cast<PCB_VIA*>( item );
            PCB_SHAPE shape( nullptr, SHAPE_T::CIRCLE );
            shape.SetPosition( via->GetPosition() );
            shape.SetRadius( via->GetWidth() / 2 + pcbIUScale.mmToIU( 0.06 ) );
            highlights.push_back( std::move( shape ) );
        }
    }

    aMarker.SetItemHighlights( std::move( highlights ) );
}


void PANEL_ONLINE_DRC::refreshList()
{
    m_list->DeleteAllItems();
    m_listMarkerIds.clear();
    long count = 0;

    if( m_board )
    {
        for( PCB_MARKER* marker : m_board->Markers() )
        {
            if( !marker->IsOnline() )
                continue;

            wxString severity;

            switch( marker->GetSeverity() )
            {
            case RPT_SEVERITY_ERROR:     severity = _( "Error" );    break;
            case RPT_SEVERITY_WARNING:   severity = _( "Warning" );  break;
            case RPT_SEVERITY_EXCLUSION: severity = _( "Excluded" ); break;
            default:                     severity = _( "Info" );     break;
            }

            long row = m_list->InsertItem( count, severity );
            m_list->SetItem( row, 1, HYPERLINK_DV_RENDERER::StripMarkup(
                                             marker->GetRCItem()->GetErrorMessage( true ) ) );
            m_listMarkerIds.push_back( marker->m_Uuid );
            ++count;
        }
    }

    m_status->SetLabel( wxString::Format( count == 1 ? _( "Online DRC: %ld violation" )
                                                        : _( "Online DRC: %ld violations" ),
                                          count ) );
}


void PANEL_ONLINE_DRC::onItemActivated( wxListEvent& aEvent )
{
    long row = aEvent.GetIndex();

    if( row < 0 || static_cast<size_t>( row ) >= m_listMarkerIds.size() || !m_board )
        return;

    PCB_MARKER* marker = dynamic_cast<PCB_MARKER*>(
            m_board->ResolveItem( m_listMarkerIds[static_cast<size_t>( row )], true ) );

    if( !marker || !marker->IsOnline() )
        return;

    std::vector<BOARD_ITEM*> items;

    for( const KIID& id : marker->GetRCItem()->GetIDs() )
    {
        if( BOARD_ITEM* item = m_board->ResolveItem( id, true ) )
            items.push_back( item );
    }

    items.push_back( marker );
    m_frame->FocusOnItems( items, marker->GetLayer(), true );
}


void PANEL_ONLINE_DRC::onBoardChanging( wxCommandEvent& aEvent )
{
    attachToBoard( nullptr );
    ++m_generation;
    m_dirty = false;
    m_debounceTimer.Stop();

    if( m_cancel )
        m_cancel->store( true, std::memory_order_relaxed );

    m_list->DeleteAllItems();
    m_listMarkerIds.clear();
    aEvent.Skip();
}


void PANEL_ONLINE_DRC::onBoardChanged( wxCommandEvent& aEvent )
{
    attachToBoard( m_frame->GetBoard() );
    schedule();
    aEvent.Skip();
}


bool PANEL_ONLINE_DRC::containsBoardGeometry( const std::vector<BOARD_ITEM*>& aItems )
{
    return std::any_of( aItems.begin(), aItems.end(),
                        []( const BOARD_ITEM* aItem )
                        {
                            return aItem && aItem->Type() != PCB_MARKER_T;
                        } );
}


void PANEL_ONLINE_DRC::OnBoardItemAdded( BOARD&, BOARD_ITEM* aItem )
{
    if( !m_applyingResults && aItem && aItem->Type() != PCB_MARKER_T )
        schedule();
}


void PANEL_ONLINE_DRC::OnBoardItemsAdded( BOARD&, std::vector<BOARD_ITEM*>& aItems )
{
    if( !m_applyingResults && containsBoardGeometry( aItems ) )
        schedule();
}


void PANEL_ONLINE_DRC::OnBoardItemRemoved( BOARD&, BOARD_ITEM* aItem )
{
    if( !m_applyingResults && aItem && aItem->Type() != PCB_MARKER_T )
        schedule();
}


void PANEL_ONLINE_DRC::OnBoardItemsRemoved( BOARD&, std::vector<BOARD_ITEM*>& aItems )
{
    if( !m_applyingResults && containsBoardGeometry( aItems ) )
        schedule();
}


void PANEL_ONLINE_DRC::OnBoardNetSettingsChanged( BOARD& )
{
    if( !m_applyingResults )
        schedule();
}


void PANEL_ONLINE_DRC::OnBoardItemChanged( BOARD&, BOARD_ITEM* aItem )
{
    if( !m_applyingResults && aItem && aItem->Type() != PCB_MARKER_T )
        schedule();
}


void PANEL_ONLINE_DRC::OnBoardItemsChanged( BOARD&, std::vector<BOARD_ITEM*>& aItems )
{
    if( !m_applyingResults && containsBoardGeometry( aItems ) )
        schedule();
}


void PANEL_ONLINE_DRC::OnBoardCompositeUpdate( BOARD&, std::vector<BOARD_ITEM*>& aAdded,
                                                std::vector<BOARD_ITEM*>& aRemoved,
                                                std::vector<BOARD_ITEM*>& aChanged )
{
    if( !m_applyingResults
            && ( containsBoardGeometry( aAdded ) || containsBoardGeometry( aRemoved )
                 || containsBoardGeometry( aChanged ) ) )
        schedule();
}
