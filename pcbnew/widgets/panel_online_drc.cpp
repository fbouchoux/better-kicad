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

#include "panel_online_drc.h"

#include <algorithm>

#include <board.h>
#include <layer_ids.h>
#include <math/util.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_edit_frame.h>
#include <view/view.h>
#include <widgets/wx_data_view_hyperlink_renderer.h>
#include <wx/sizer.h>
#include <wx/stattext.h>


PANEL_ONLINE_DRC::PANEL_ONLINE_DRC( PCB_EDIT_FRAME* aFrame, LIVE_DRC* aLiveDRC ) :
        wxPanel( aFrame ),
        m_frame( aFrame ),
        m_liveDRC( aLiveDRC )
{
    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );

    m_status = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_list = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES );
    m_list->AppendColumn( _( "Severity" ), wxLIST_FORMAT_LEFT, FromDIP( 90 ) );
    m_list->AppendColumn( _( "Violation" ), wxLIST_FORMAT_LEFT, FromDIP( 520 ) );

    sizer->Add( m_status, 0, wxEXPAND | wxALL, FromDIP( 5 ) );
    sizer->Add( m_list, 1, wxEXPAND );
    SetSizer( sizer );

    m_list->Bind( wxEVT_LIST_ITEM_ACTIVATED, &PANEL_ONLINE_DRC::onItemActivated, this );
    m_frame->GetCanvas()->Bind( wxEVT_MOTION, &PANEL_ONLINE_DRC::onCanvasMotion, this );

    m_liveDRC->SetChangedHandler( [this]() { refresh(); } );
    refresh();
}


PANEL_ONLINE_DRC::~PANEL_ONLINE_DRC()
{
    Shutdown();
}


void PANEL_ONLINE_DRC::Shutdown()
{
    if( !m_liveDRC )
        return;

    m_liveDRC->SetChangedHandler( nullptr );
    m_liveDRC = nullptr;

    m_frame->GetCanvas()->Unbind( wxEVT_MOTION, &PANEL_ONLINE_DRC::onCanvasMotion, this );
    m_frame->GetCanvas()->SetToolTip( wxString() );
    m_canvasTooltip.clear();
}


void PANEL_ONLINE_DRC::refresh()
{
    m_status->SetLabel( m_liveDRC->GetStatus() );

    std::shared_ptr<const LIVE_DRC_RESULTS> results = m_liveDRC->GetResults();

    if( results == m_shown )
        return;

    m_shown = results;
    m_list->DeleteAllItems();

    if( !m_shown )
        return;

    long row = 0;

    for( const LIVE_DRC_VIOLATION& violation : *m_shown )
    {
        wxString severity = violation.severity == RPT_SEVERITY_ERROR ? _( "Error" ) : _( "Warning" );
        wxString message = violation.marker->GetRCItem()->GetErrorMessage( true );

        m_list->InsertItem( row, severity );
        m_list->SetItem( row, 1, HYPERLINK_DV_RENDERER::StripMarkup( message ) );
        ++row;
    }
}


void PANEL_ONLINE_DRC::onItemActivated( wxListEvent& aEvent )
{
    long row = aEvent.GetIndex();

    if( !m_shown || row < 0 || static_cast<size_t>( row ) >= m_shown->size() )
        return;

    const PCB_MARKER& marker = *( *m_shown )[row].marker;
    BOARD*            board = m_frame->GetBoard();
    std::vector<BOARD_ITEM*> items;

    for( const KIID& id : marker.GetRCItem()->GetIDs() )
    {
        if( BOARD_ITEM* item = board->ResolveItem( id, true ) )
            items.push_back( item );
    }

    if( !items.empty() )
        m_frame->FocusOnItems( items, marker.GetLayer() );
    else
        m_frame->FocusOnLocation( marker.GetPosition() );
}


void PANEL_ONLINE_DRC::onCanvasMotion( wxMouseEvent& aEvent )
{
    aEvent.Skip();

    wxString     tooltip;
    KIGFX::VIEW* view = m_frame->GetCanvas()->GetView();

    if( m_shown && !aEvent.Dragging() )
    {
        VECTOR2D world = view->ToWorld( VECTOR2D( aEvent.GetX(), aEvent.GetY() ) );
        VECTOR2I cursor( KiROUND( world.x ), KiROUND( world.y ) );
        int      accuracy = std::max( 1, KiROUND( view->ToWorld( 4.0 ) ) );

        for( const LIVE_DRC_VIOLATION& violation : *m_shown )
        {
            int layer = violation.severity == RPT_SEVERITY_ERROR ? LAYER_DRC_ERROR : LAYER_DRC_WARNING;

            if( view->IsLayerVisible( layer ) && violation.marker->HitTest( cursor, accuracy ) )
            {
                tooltip = HYPERLINK_DV_RENDERER::StripMarkup(
                        violation.marker->GetRCItem()->GetErrorMessage( true ) );
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
