/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
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

/**
 * @file test_pcb_render_settings.cpp
 * Test suite for PCB_RENDER_SETTINGS
 */

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <pcb_painter.h>
#include <pcb_point.h>
#include <layer_ids.h>
#include <settings/color_settings.h>
#include <board.h>
#include <base_units.h>
#include <pcb_track.h>
#include <pcb_view.h>
#include <footprint.h>
#include <pad.h>
#include <gal/gal_display_options.h>
#include <gal/graphics_abstraction_layer.h>

class TEST_PCB_RENDER_SETTINGS_FIXTURE
{
public:
    KIGFX::PCB_RENDER_SETTINGS m_settings;
    COLOR_SETTINGS             m_colorSettings;

    TEST_PCB_RENDER_SETTINGS_FIXTURE()
    {
        m_colorSettings.SetColor( LAYER_POINTS, KIGFX::COLOR4D( 1.0, 0.0, 1.0, 1.0 ) );

        for( int i = 0; i < PCB_LAYER_ID_COUNT; i++ )
            m_colorSettings.SetColor( i, KIGFX::COLOR4D( 0.5, 0.5, 0.5, 1.0 ) );

        m_settings.LoadColors( &m_colorSettings );
    }
};


BOOST_FIXTURE_TEST_SUITE( PcbRenderSettings, TEST_PCB_RENDER_SETTINGS_FIXTURE )


/**
 * Test that PCB_RENDER_SETTINGS::GetColor returns a visible color for POINT layers.
 *
 * This is a regression test for issue #22634 where pressing ESC caused footprint
 * points to vanish because UpdateAllLayersColor() was requesting the color for
 * the virtual POINT layer (LAYER_POINT_START + boardLayer), which had no color
 * defined and returned transparent.
 */
BOOST_AUTO_TEST_CASE( PointLayerColorIsVisible )
{
    PCB_POINT point( nullptr );
    point.SetLayer( F_SilkS );

    int pointLayer = POINT_LAYER_FOR( F_SilkS );

    KIGFX::COLOR4D color = m_settings.GetColor( &point, pointLayer );

    BOOST_CHECK_MESSAGE( color.a > 0.0,
                         "Color for POINT layer should not be transparent (alpha was "
                         + std::to_string( color.a ) + ")" );

    BOOST_CHECK_MESSAGE( color == m_colorSettings.GetColor( LAYER_POINTS ),
                         "POINT layer color should match LAYER_POINTS color" );
}


/**
 * Test that multiple board layers all map to LAYER_POINTS color.
 */
BOOST_AUTO_TEST_CASE( AllPointLayersMapToLayerPoints )
{
    KIGFX::COLOR4D expectedColor = m_colorSettings.GetColor( LAYER_POINTS );

    std::vector<PCB_LAYER_ID> testLayers = { F_Cu, B_Cu, F_SilkS, B_SilkS, Edge_Cuts, User_1 };

    for( PCB_LAYER_ID boardLayer : testLayers )
    {
        PCB_POINT point( nullptr );
        point.SetLayer( boardLayer );

        int pointLayer = POINT_LAYER_FOR( boardLayer );
        KIGFX::COLOR4D color = m_settings.GetColor( &point, pointLayer );

        BOOST_CHECK_MESSAGE( color == expectedColor,
                             "POINT layer for " + std::to_string( boardLayer )
                             + " should have LAYER_POINTS color" );
    }
}


/**
 * Test that smart pad coloring distinguishes electrical roles without changing tracks.
 */
BOOST_AUTO_TEST_CASE( SmartPadColorsUseElectricalRole )
{
    const KIGFX::COLOR4D layerColor( 0.25, 0.45, 0.7, 1.0 );
    m_settings.SetLayerColor( F_Cu, layerColor );
    m_settings.SetSmartPadColorMode( true );

    BOARD board;

    // Create representative ground, power and signal nets
    NETINFO_ITEM* groundNet = new NETINFO_ITEM( &board, wxT( "/power/AGND" ), 1 );
    NETINFO_ITEM* powerNet = new NETINFO_ITEM( &board, wxT( "+3V3" ), 2 );
    NETINFO_ITEM* signalNet = new NETINFO_ITEM( &board, wxT( "SCLK" ), 3 );
    board.Add( groundNet );
    board.Add( powerNet );
    board.Add( signalNet );

    FOOTPRINT* footprint = new FOOTPRINT( &board );
    board.Add( footprint );

    // Add one pad for each role and leave the no-connect pad without a net
    auto addPad = [&]( const wxString& aPinType, NETINFO_ITEM* aNet )
    {
        PAD* pad = new PAD( footprint );
        pad->SetAttribute( PAD_ATTRIB::SMD );
        pad->SetLayer( F_Cu );
        pad->SetPinType( aPinType );

        if( aNet )
            pad->SetNet( aNet );

        footprint->Add( pad );
        return pad;
    };

    PAD* groundPad = addPad( wxT( "power_in" ), groundNet );
    PAD* powerPad = addPad( wxT( "passive" ), powerNet );
    PAD* signalPad = addPad( wxT( "input" ), signalNet );
    PAD* noConnectPad = addPad( wxT( "passive" ), nullptr );
    noConnectPad->SetPinFunction( wxT( "NC" ) );
    PAD* typedNoConnectPad = addPad( wxT( "input+no_connect" ), nullptr );

    KIGFX::COLOR4D groundColor =
            m_settings.GetColor( groundPad, PAD_COPPER_LAYER_FOR( F_Cu ) );
    KIGFX::COLOR4D powerColor =
            m_settings.GetColor( powerPad, PAD_COPPER_LAYER_FOR( F_Cu ) );
    KIGFX::COLOR4D signalColor =
            m_settings.GetColor( signalPad, PAD_COPPER_LAYER_FOR( F_Cu ) );
    KIGFX::COLOR4D noConnectColor =
            m_settings.GetColor( noConnectPad, PAD_COPPER_LAYER_FOR( F_Cu ) );
    KIGFX::COLOR4D typedNoConnectColor =
            m_settings.GetColor( typedNoConnectPad, PAD_COPPER_LAYER_FOR( F_Cu ) );

    BOOST_CHECK( powerColor.GetBrightness() > layerColor.GetBrightness() );
    BOOST_CHECK( groundColor.GetBrightness() < layerColor.GetBrightness() );
    BOOST_CHECK( noConnectColor.GetBrightness() < groundColor.GetBrightness() );
    BOOST_CHECK( typedNoConnectColor == noConnectColor );
    BOOST_CHECK( signalColor == layerColor );

    // A power-net track must retain the unmodified copper layer color
    PCB_TRACK track( &board );
    track.SetLayer( F_Cu );
    track.SetNet( powerNet );
    BOOST_CHECK( m_settings.GetColor( &track, F_Cu ) == layerColor );
}


BOOST_AUTO_TEST_SUITE_END()


namespace
{

class RECORDING_GAL : public KIGFX::GAL
{
public:
    explicit RECORDING_GAL( KIGFX::GAL_DISPLAY_OPTIONS& aOptions ) :
            GAL( aOptions )
    {
    }

    int BeginGroup() override
    {
        m_openGroup = m_nextGroup++;
        m_groups[m_openGroup].clear();
        return m_openGroup;
    }

    void EndGroup() override { m_openGroup = -1; }

    void DeleteGroup( int aGroupNumber ) override { m_groups.erase( aGroupNumber ); }

    void ClearCache() override { m_groups.clear(); }

    void DrawCircle( const VECTOR2D&, double ) override { record(); }

    void DrawArc( const VECTOR2D&, double, const EDA_ANGLE&, const EDA_ANGLE& ) override { record(); }

    // Mirrors the real backends, which rewrite the colour of every vertex in the group.
    void ChangeGroupColor( int aGroupNumber, const KIGFX::COLOR4D& aNewColor ) override
    {
        auto it = m_groups.find( aGroupNumber );

        if( it != m_groups.end() )
            it->second.assign( it->second.size(), aNewColor );
    }

    bool HasGroupWith( const KIGFX::COLOR4D& aFirst, const KIGFX::COLOR4D& aSecond ) const
    {
        for( const auto& [groupNumber, colors] : m_groups )
        {
            if( std::find( colors.begin(), colors.end(), aFirst ) != colors.end()
                && std::find( colors.begin(), colors.end(), aSecond ) != colors.end() )
            {
                return true;
            }
        }

        return false;
    }

private:
    void record()
    {
        if( m_openGroup >= 0 )
            m_groups[m_openGroup].push_back( GetIsFill() ? GetFillColor() : GetStrokeColor() );
    }

    std::map<int, std::vector<KIGFX::COLOR4D>> m_groups;
    int                                        m_nextGroup = 1;
    int                                        m_openGroup = -1;
};

} // namespace


BOOST_AUTO_TEST_SUITE( PcbViaHoleRecolour )


BOOST_AUTO_TEST_CASE( BlindViaHoleKeepsLayerPairColours )
{
    const KIGFX::COLOR4D topColor( 1.0, 0.0, 0.0, 1.0 );
    const KIGFX::COLOR4D bottomColor( 0.0, 1.0, 0.0, 1.0 );

    KIGFX::GAL_DISPLAY_OPTIONS options;
    RECORDING_GAL              gal( options );

    // view must outlive board so board items unregister from a live view at teardown.
    KIGFX::PCB_VIEW    view;
    KIGFX::PCB_PAINTER painter( &gal, FRAME_PCB_EDITOR );
    BOARD              board;

    KIGFX::PCB_RENDER_SETTINGS* settings = painter.GetSettings();
    settings->SetLayerColor( F_Cu, topColor );
    settings->SetLayerColor( In1_Cu, bottomColor );
    settings->SetLayerColor( LAYER_VIA_HOLES, KIGFX::COLOR4D( 1.0, 0.8, 0.0, 1.0 ) );

    view.SetGAL( &gal );
    view.SetPainter( &painter );

    board.SetCopperLayerCount( 4 );

    PCB_VIA* via = new PCB_VIA( &board );
    via->SetPadstackMode( PADSTACK::MODE::NORMAL );
    via->SetViaType( VIATYPE::BLIND );
    via->SetLayerPair( F_Cu, In1_Cu );
    via->SetStart( { 0, 0 } );
    via->SetEnd( { 0, 0 } );
    via->SetWidth( PADSTACK::ALL_LAYERS, pcbIUScale.mmToIU( 0.8 ) );
    via->SetDrill( pcbIUScale.mmToIU( 0.4 ) );
    board.Add( via );

    view.Add( via );
    view.UpdateItems();

    BOOST_REQUIRE_MESSAGE( gal.HasGroupWith( topColor, bottomColor ),
                           "painter did not emit a two colour via hole to begin with" );

    view.UpdateAllLayersColor();

    BOOST_CHECK_MESSAGE( gal.HasGroupWith( topColor, bottomColor ),
                         "recolouring flattened the blind via hole to a single colour" );
}


BOOST_AUTO_TEST_CASE( BackdrilledViaKeepsIndicatorColours )
{
    const KIGFX::COLOR4D startColor( 1.0, 0.0, 0.0, 1.0 );
    const KIGFX::COLOR4D endColor( 0.0, 1.0, 0.0, 1.0 );

    KIGFX::GAL_DISPLAY_OPTIONS options;
    RECORDING_GAL              gal( options );

    // view must outlive board so board items unregister from a live view at teardown.
    KIGFX::PCB_VIEW    view;
    KIGFX::PCB_PAINTER painter( &gal, FRAME_PCB_EDITOR );
    BOARD              board;

    KIGFX::PCB_RENDER_SETTINGS* settings = painter.GetSettings();
    settings->SetLayerColor( F_Cu, startColor );
    settings->SetLayerColor( In1_Cu, endColor );
    settings->SetLayerColor( LAYER_VIA_HOLEWALLS, KIGFX::COLOR4D( 1.0, 0.8, 0.0, 1.0 ) );

    view.SetGAL( &gal );
    view.SetPainter( &painter );

    board.SetCopperLayerCount( 4 );

    PCB_VIA* via = new PCB_VIA( &board );
    via->SetPadstackMode( PADSTACK::MODE::NORMAL );
    via->SetLayerPair( F_Cu, B_Cu );
    via->SetStart( { 0, 0 } );
    via->SetEnd( { 0, 0 } );
    via->SetWidth( PADSTACK::ALL_LAYERS, pcbIUScale.mmToIU( 0.8 ) );
    via->SetDrill( pcbIUScale.mmToIU( 0.4 ) );
    via->SetSecondaryDrillSize( std::optional<int>( pcbIUScale.mmToIU( 0.6 ) ) );
    via->SetSecondaryDrillStartLayer( F_Cu );
    via->SetSecondaryDrillEndLayer( In1_Cu );
    board.Add( via );

    view.Add( via );
    view.UpdateItems();

    BOOST_REQUIRE_MESSAGE( gal.HasGroupWith( startColor, endColor ),
                           "painter did not emit a two colour backdrill indicator to begin with" );

    view.UpdateAllLayersColor();

    BOOST_CHECK_MESSAGE( gal.HasGroupWith( startColor, endColor ),
                         "recolouring flattened the backdrill indicator to a single colour" );
}


BOOST_AUTO_TEST_CASE( BackdrilledPadKeepsIndicatorColours )
{
    const KIGFX::COLOR4D startColor( 1.0, 0.0, 0.0, 1.0 );
    const KIGFX::COLOR4D endColor( 0.0, 1.0, 0.0, 1.0 );

    KIGFX::GAL_DISPLAY_OPTIONS options;
    RECORDING_GAL              gal( options );

    // view must outlive board so board items unregister from a live view at teardown.
    KIGFX::PCB_VIEW    view;
    KIGFX::PCB_PAINTER painter( &gal, FRAME_PCB_EDITOR );
    BOARD              board;

    KIGFX::PCB_RENDER_SETTINGS* settings = painter.GetSettings();
    settings->SetLayerColor( F_Cu, startColor );
    settings->SetLayerColor( In1_Cu, endColor );
    settings->SetLayerColor( LAYER_PAD_HOLEWALLS, KIGFX::COLOR4D( 1.0, 0.8, 0.0, 1.0 ) );

    view.SetGAL( &gal );
    view.SetPainter( &painter );

    board.SetCopperLayerCount( 4 );

    FOOTPRINT* footprint = new FOOTPRINT( &board );
    board.Add( footprint );

    PAD* pad = new PAD( footprint );
    pad->SetAttribute( PAD_ATTRIB::PTH );
    pad->SetPosition( { 0, 0 } );
    pad->SetSizeX( pcbIUScale.mmToIU( 1.6 ) );
    pad->SetSizeY( pcbIUScale.mmToIU( 1.6 ) );
    pad->SetDrillSizeX( pcbIUScale.mmToIU( 0.8 ) );
    pad->SetDrillSizeY( pcbIUScale.mmToIU( 0.8 ) );
    pad->SetSecondaryDrillSizeX( pcbIUScale.mmToIU( 1.2 ) );
    pad->SetSecondaryDrillSizeY( pcbIUScale.mmToIU( 1.2 ) );
    pad->SetSecondaryDrillStartLayer( F_Cu );
    pad->SetSecondaryDrillEndLayer( In1_Cu );
    footprint->Add( pad );

    view.Add( pad );
    view.UpdateItems();

    BOOST_REQUIRE_MESSAGE( gal.HasGroupWith( startColor, endColor ),
                           "painter did not emit a two colour pad backdrill to begin with" );

    view.UpdateAllLayersColor();

    BOOST_CHECK_MESSAGE( gal.HasGroupWith( startColor, endColor ),
                         "recolouring flattened the pad backdrill indicator to a single colour" );
}


BOOST_AUTO_TEST_SUITE_END()
