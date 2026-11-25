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

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <board.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_track.h>
#include <pcbnew_utils/board_test_utils.h>
#include <settings/settings_manager.h>
#include <tools/pcb_control.h>

#include <router/pns_itemset.h>
#include <router/pns_kicad_iface.h>
#include <router/pns_line.h>
#include <router/pns_line_placer.h>
#include <router/pns_node.h>
#include <router/pns_placement_algo.h>
#include <router/pns_router.h>
#include <router/pns_routing_settings.h>
#include <router/pns_sizes_settings.h>
#include <router/pns_via.h>

#include <memory>


/**
 * Regression coverage for https://gitlab.com/kicad/code/kicad/-/issues/24772
 *
 * A through via placed while routing a differential pair took its span from the router layer
 * pair instead of the whole board.  The via stopped short of the outer layers, which removed
 * the outer annular rings and made the annular width tests report zero.
 */
struct VIA_LAYER_SPAN_FIXTURE
{
    SETTINGS_MANAGER       m_settingsManager;
    std::unique_ptr<BOARD> m_board;
};


BOOST_FIXTURE_TEST_CASE( DiffPairThroughViaSpansWholeBoard, VIA_LAYER_SPAN_FIXTURE )
{
    KI_TEST::LoadBoard( m_settingsManager, "issue24772/fw16_MCIO8i", m_board );
    BOOST_REQUIRE( m_board );
    BOOST_REQUIRE_EQUAL( m_board->GetCopperLayerCount(), 6 );

    FOOTPRINT* connector = m_board->FindFootprintByReference( wxT( "MCIO1" ) );
    BOOST_REQUIRE( connector );

    PAD* padP = connector->FindPadByNumber( wxT( "B5" ) );
    BOOST_REQUIRE( padP );

    PNS::ROUTER           router;
    PNS_KICAD_IFACE_BASE  iface;
    PNS::ROUTING_SETTINGS routingSettings( nullptr, "" );

    iface.SetBoard( m_board.get() );
    router.SetInterface( &iface );
    router.ClearWorld();
    router.SyncWorld();
    router.LoadSettings( &routingSettings );
    router.SetMode( PNS::PNS_MODE_ROUTE_DIFF_PAIR );

    // The reporter routed with a layer pair of In1.Cu and In4.Cu, which is what exposed the bug
    const int pnsTop = iface.GetPNSLayerFromBoardLayer( In1_Cu );
    const int pnsBottom = iface.GetPNSLayerFromBoardLayer( In4_Cu );
    const int pnsFront = iface.GetPNSLayerFromBoardLayer( F_Cu );
    const int pnsBack = iface.GetPNSLayerFromBoardLayer( B_Cu );

    BOOST_REQUIRE_NE( pnsTop, pnsFront );
    BOOST_REQUIRE_NE( pnsBottom, pnsBack );

    const VECTOR2I startPoint = padP->GetPosition();
    PNS::ITEM*     startItem = router.GetWorld()->FindItemByParent( padP );

    BOOST_REQUIRE( startItem );

    PNS::SIZES_SETTINGS sizes( router.Sizes() );
    iface.SetStartLayerFromPCBNew( F_Cu );
    BOOST_REQUIRE( iface.ImportSizes( sizes, startItem, nullptr, startPoint ) );

    sizes.ClearLayerPairs();
    sizes.AddLayerPair( pnsTop, pnsBottom );
    sizes.SetViaType( VIATYPE::THROUGH );
    router.UpdateSizes( sizes );

    router.Settings().SetAllowDRCViolations( true );

    BOOST_REQUIRE( router.StartRouting( startPoint, startItem, pnsFront ) );

    router.ToggleViaPlacement();
    BOOST_REQUIRE( router.IsPlacingVia() );

    // Pull the head away from the connector so the placer has room to build a trace with a via
    const VECTOR2I endPoint = startPoint + VECTOR2I( 0, pcbIUScale.mmToIU( 5.0 ) );
    BOOST_REQUIRE( router.Move( endPoint, nullptr ) );

    const PNS::VIA* headVia = nullptr;

    const PNS::ITEM_SET traces = router.Placer()->Traces();

    for( const PNS::ITEM* item : traces.CItems() )
    {
        if( item->Kind() != PNS::ITEM::LINE_T )
            continue;

        const PNS::LINE* line = static_cast<const PNS::LINE*>( item );

        if( line->EndsWithVia() )
        {
            headVia = &line->Via();
            break;
        }
    }

    BOOST_REQUIRE( headVia );
    BOOST_CHECK( headVia->ViaType() == VIATYPE::THROUGH );

    // Before the fix the span followed the layer pair, so the via missed F.Cu and B.Cu
    BOOST_CHECK_EQUAL( headVia->Layers().Start(), pnsFront );
    BOOST_CHECK_EQUAL( headVia->Layers().End(), pnsBack );
    BOOST_CHECK_EQUAL( headVia->HoleLayers().Start(), pnsFront );
    BOOST_CHECK_EQUAL( headVia->HoleLayers().End(), pnsBack );

    router.StopRouting();
}


/**
 * A Smart Via placed directly at the routing start has no track segments.  It must still leave
 * the line placer active so the route can continue on the requested destination layer.
 */
BOOST_FIXTURE_TEST_CASE( ViaOnlyTransitionCanContinueRouting, VIA_LAYER_SPAN_FIXTURE )
{
    KI_TEST::LoadBoard( m_settingsManager, "issue24772/fw16_MCIO8i", m_board );
    BOOST_REQUIRE( m_board );

    FOOTPRINT* connector = m_board->FindFootprintByReference( wxT( "MCIO1" ) );
    BOOST_REQUIRE( connector );

    PAD* pad = connector->FindPadByNumber( wxT( "B5" ) );
    BOOST_REQUIRE( pad );

    PNS::ROUTER           router;
    PNS_KICAD_IFACE_BASE  iface;
    PNS::ROUTING_SETTINGS routingSettings( nullptr, "" );

    // Walkaround would push the via off the pad, away from the neighbouring pins
    routingSettings.SetMode( PNS::RM_MarkObstacles );

    iface.SetBoard( m_board.get() );
    router.SetInterface( &iface );
    router.ClearWorld();
    router.SyncWorld();
    router.LoadSettings( &routingSettings );
    router.SetMode( PNS::PNS_MODE_ROUTE_SINGLE );

    const int      pnsFront = iface.GetPNSLayerFromBoardLayer( F_Cu );
    const int      pnsTarget = iface.GetPNSLayerFromBoardLayer( In1_Cu );
    const VECTOR2I startPoint = pad->GetPosition();
    PNS::ITEM*     startItem = router.GetWorld()->FindItemByParent( pad );

    BOOST_REQUIRE( startItem );

    PNS::SIZES_SETTINGS sizes( router.Sizes() );
    iface.SetStartLayerFromPCBNew( F_Cu );
    BOOST_REQUIRE( iface.ImportSizes( sizes, startItem, nullptr, startPoint ) );

    sizes.ClearLayerPairs();
    sizes.AddLayerPair( pnsFront, pnsTarget );
    sizes.SetViaType( VIATYPE::THROUGH );
    router.UpdateSizes( sizes );
    router.Settings().SetAllowDRCViolations( true );

    BOOST_REQUIRE( router.StartRouting( startPoint, startItem, pnsFront ) );
    router.ToggleViaPlacement();
    BOOST_REQUIRE( router.Move( startPoint, startItem ) );

    PNS::LINE_PLACER* placer = dynamic_cast<PNS::LINE_PLACER*>( router.Placer() );
    BOOST_REQUIRE( placer );

    // The head may snap a few microns onto the hull of a neighbouring pin
    const PNS::LINE head = placer->Trace();
    BOOST_REQUIRE( head.EndsWithVia() );
    BOOST_REQUIRE_LT( ( head.Via().Pos() - startPoint ).EuclideanNorm(), pcbIUScale.mmToIU( 0.1 ) );

    const VECTOR2I viaPos = head.Via().Pos();

    placer->SetContinueAfterVia( true );
    BOOST_CHECK( !router.FixRoute( startPoint, startItem, false, false ) );
    placer->SetContinueAfterVia( false );

    BOOST_CHECK( router.RoutingInProgress() );
    BOOST_CHECK( !router.IsPlacingVia() );
    BOOST_REQUIRE( router.SwitchLayer( pnsTarget ) );
    BOOST_CHECK_EQUAL( router.GetCurrentLayer(), pnsTarget );

    const VECTOR2I endPoint = startPoint + VECTOR2I( 0, pcbIUScale.mmToIU( 5.0 ) );
    BOOST_REQUIRE( router.Move( endPoint, nullptr ) );
    BOOST_CHECK_EQUAL( router.Placer()->CurrentStart(), viaPos );
    BOOST_CHECK_EQUAL( router.Placer()->CurrentLayer(), pnsTarget );

    bool                hasTargetTrace = false;
    const PNS::ITEM_SET traces = router.Placer()->Traces();

    for( const PNS::ITEM* item : traces.CItems() )
    {
        if( item->Kind() == PNS::ITEM::LINE_T )
        {
            const PNS::LINE* line = static_cast<const PNS::LINE*>( item );
            hasTargetTrace = line->Layer() == pnsTarget && line->SegmentCount() > 0;
        }
    }

    BOOST_CHECK( hasTargetTrace );

    // Undo takes the via back and resumes placing it on the source layer
    router.UndoLastSegment();
    BOOST_CHECK( router.IsPlacingVia() );
    BOOST_CHECK_EQUAL( router.GetCurrentLayer(), pnsFront );
    BOOST_CHECK_EQUAL( router.Placer()->CurrentStart(), startPoint );

    router.StopRouting();
}


/**
 * Replacing an active Smart Via can change a fallback through via into a preset microvia.  The
 * preview must update the via type and layer span, not only its diameter and drill.
 */
BOOST_FIXTURE_TEST_CASE( ActiveViaPreviewUpdatesTypeAndSpan, VIA_LAYER_SPAN_FIXTURE )
{
    KI_TEST::LoadBoard( m_settingsManager, "issue24772/fw16_MCIO8i", m_board );
    BOOST_REQUIRE( m_board );

    FOOTPRINT* connector = m_board->FindFootprintByReference( wxT( "MCIO1" ) );
    BOOST_REQUIRE( connector );

    PAD* pad = connector->FindPadByNumber( wxT( "B5" ) );
    BOOST_REQUIRE( pad );

    PNS::ROUTER           router;
    PNS_KICAD_IFACE_BASE  iface;
    PNS::ROUTING_SETTINGS routingSettings( nullptr, "" );

    // Walkaround drops a via that would land on the neighbouring pins
    routingSettings.SetMode( PNS::RM_MarkObstacles );

    iface.SetBoard( m_board.get() );
    router.SetInterface( &iface );
    router.ClearWorld();
    router.SyncWorld();
    router.LoadSettings( &routingSettings );
    router.SetMode( PNS::PNS_MODE_ROUTE_SINGLE );

    const int      pnsFront = iface.GetPNSLayerFromBoardLayer( F_Cu );
    const int      pnsFallback = iface.GetPNSLayerFromBoardLayer( In3_Cu );
    const int      pnsTarget = iface.GetPNSLayerFromBoardLayer( In2_Cu );
    const VECTOR2I startPoint = pad->GetPosition();
    PNS::ITEM*     startItem = router.GetWorld()->FindItemByParent( pad );

    BOOST_REQUIRE( startItem );

    PNS::SIZES_SETTINGS sizes( router.Sizes() );
    iface.SetStartLayerFromPCBNew( F_Cu );
    BOOST_REQUIRE( iface.ImportSizes( sizes, startItem, nullptr, startPoint ) );
    sizes.ClearLayerPairs();
    sizes.AddLayerPair( pnsFront, pnsFallback );
    sizes.SetViaType( VIATYPE::THROUGH );
    router.UpdateSizes( sizes );
    router.Settings().SetAllowDRCViolations( true );

    BOOST_REQUIRE( router.StartRouting( startPoint, startItem, pnsFront ) );
    router.ToggleViaPlacement();

    const VECTOR2I endPoint = startPoint + VECTOR2I( 0, pcbIUScale.mmToIU( 5.0 ) );
    BOOST_REQUIRE( router.Move( endPoint, nullptr ) );

    sizes.SetViaDiameter( pcbIUScale.mmToIU( 0.2 ) );
    sizes.SetViaDrill( pcbIUScale.mmToIU( 0.1 ) );
    sizes.SetViaType( VIATYPE::MICROVIA );
    sizes.ClearLayerPairs();
    sizes.AddLayerPair( pnsFront, pnsTarget );
    router.UpdateSizes( sizes );

    const PNS::VIA*     headVia = nullptr;
    const PNS::ITEM_SET traces = router.Placer()->Traces();

    for( const PNS::ITEM* item : traces.CItems() )
    {
        if( item->Kind() != PNS::ITEM::LINE_T )
            continue;

        const PNS::LINE* line = static_cast<const PNS::LINE*>( item );

        if( line->EndsWithVia() )
        {
            headVia = &line->Via();
            break;
        }
    }

    // Checked before any Move(), which would rebuild the via on its own
    BOOST_REQUIRE( headVia );
    BOOST_CHECK( headVia->ViaType() == VIATYPE::MICROVIA );
    BOOST_CHECK_EQUAL( headVia->Diameter( pnsFront ), pcbIUScale.mmToIU( 0.2 ) );
    BOOST_CHECK_EQUAL( headVia->Layers().Start(), pnsFront );
    BOOST_CHECK_EQUAL( headVia->Layers().End(), pnsTarget );

    router.StopRouting();
}


/**
 * Next and previous layer stepping skips hidden layers and stops at the ends of the stack.
 */
BOOST_FIXTURE_TEST_CASE( AdjacentCopperLayerDoesNotWrap, VIA_LAYER_SPAN_FIXTURE )
{
    KI_TEST::LoadBoard( m_settingsManager, "issue24772/fw16_MCIO8i", m_board );
    BOOST_REQUIRE( m_board );
    BOOST_REQUIRE_EQUAL( m_board->GetCopperLayerCount(), 6 );

    const BOARD* board = m_board.get();

    BOOST_CHECK_EQUAL( PCB_CONTROL::AdjacentVisibleCopperLayer( board, F_Cu, true ), In1_Cu );
    BOOST_CHECK_EQUAL( PCB_CONTROL::AdjacentVisibleCopperLayer( board, F_Cu, false ), UNDEFINED_LAYER );
    BOOST_CHECK_EQUAL( PCB_CONTROL::AdjacentVisibleCopperLayer( board, B_Cu, true ), UNDEFINED_LAYER );
    BOOST_CHECK_EQUAL( PCB_CONTROL::AdjacentVisibleCopperLayer( board, B_Cu, false ), In4_Cu );

    LSET visible = m_board->GetVisibleLayers();
    visible.reset( In1_Cu );
    m_board->SetVisibleLayers( visible );

    BOOST_CHECK_EQUAL( PCB_CONTROL::AdjacentVisibleCopperLayer( board, F_Cu, true ), In2_Cu );
}


/**
 * A via holds its copper span in the primary drill layers, so the router must not write the hole
 * layers back over the layer pair.  This pins the premise for dropping that write back, that the
 * two ranges are the same range for every via the router knows about.
 */
BOOST_FIXTURE_TEST_CASE( SyncedViaHoleLayersMatchCopperLayers, VIA_LAYER_SPAN_FIXTURE )
{
    KI_TEST::LoadBoard( m_settingsManager, "issue24772/fw16_MCIO8i", m_board );
    BOOST_REQUIRE( m_board );

    PNS::ROUTER          router;
    PNS_KICAD_IFACE_BASE iface;

    iface.SetBoard( m_board.get() );
    router.SetInterface( &iface );
    router.ClearWorld();
    router.SyncWorld();

    int viaCount = 0;

    for( const PCB_TRACK* track : m_board->Tracks() )
    {
        if( track->Type() != PCB_VIA_T )
            continue;

        const PNS::ITEM* item = router.GetWorld()->FindItemByParent( track );

        if( !item || item->Kind() != PNS::ITEM::VIA_T )
            continue;

        const PNS::VIA* via = static_cast<const PNS::VIA*>( item );

        BOOST_REQUIRE_EQUAL( via->HoleLayers().Start(), via->Layers().Start() );
        BOOST_REQUIRE_EQUAL( via->HoleLayers().End(), via->Layers().End() );
        viaCount++;
    }

    BOOST_CHECK_GT( viaCount, 0 );
}
