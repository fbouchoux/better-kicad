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
#include <pcbnew_utils/board_test_utils.h>

#include <atomic>
#include <set>
#include <thread>

#include <board.h>
#include <board_design_settings.h>
#include <drc/drc_engine.h>
#include <drc/drc_exclusion.h>
#include <drc/drc_item.h>
#include <drc/drc_test_provider.h>
#include <drc/live_drc.h>
#include <pcb_marker.h>
#include <progress_reporter.h>
#include <settings/settings_manager.h>


namespace
{
class TEST_CANCEL_REPORTER : public PROGRESS_REPORTER
{
public:
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
    bool IsCancelled() const override { return m_cancelled; }

    std::atomic_bool m_cancelled = false;
};


std::multiset<std::pair<int, wxString>> describe( const LIVE_DRC_RESULTS& aResults )
{
    std::multiset<std::pair<int, wxString>> described;

    for( const LIVE_DRC_VIOLATION& violation : aResults )
    {
        const std::shared_ptr<RC_ITEM>& item = violation.marker->GetRCItem();
        described.emplace( item->GetErrorCode(), item->GetMainItemID().AsString() + item->GetAuxItemID().AsString() );
    }

    return described;
}
} // namespace


struct LIVE_DRC_FIXTURE
{
    LIVE_DRC_FIXTURE()
    {
        KI_TEST::LoadBoard( m_settingsManager, "line_ending_drc/line_ending_drc_fail", m_board );

        BOARD_DESIGN_SETTINGS& bds = m_board->GetDesignSettings();

        for( int code = 0; code < DRCE_LAST; ++code )
            bds.m_DRCSeverities[code] = SEVERITY::RPT_SEVERITY_IGNORE;

        bds.m_DRCSeverities[DRCE_CLEARANCE] = SEVERITY::RPT_SEVERITY_ERROR;
        bds.m_DRCSeverities[DRCE_SHORTING_ITEMS] = SEVERITY::RPT_SEVERITY_ERROR;
    }

    SETTINGS_MANAGER       m_settingsManager;
    std::unique_ptr<BOARD> m_board;
};


BOOST_FIXTURE_TEST_SUITE( LiveDRC, LIVE_DRC_FIXTURE )


BOOST_AUTO_TEST_CASE( ProviderFilterRunsOnlyRequestedTests )
{
    std::shared_ptr<DRC_ENGINE> engine = m_board->GetDesignSettings().m_DRCEngine;
    std::set<wxString>          tests;

    engine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I&, int,
                 const std::function<void( PCB_MARKER* )>& )
            {
                if( aItem->GetViolatingTest() )
                    tests.insert( aItem->GetViolatingTest()->GetName() );
            } );

    engine->RunTests( EDA_UNITS::MM, false, false, nullptr, { wxT( "clearance" ) } );
    engine->ClearViolationHandler();

    BOOST_CHECK( !tests.empty() );
    BOOST_CHECK( tests == std::set<wxString>{ wxT( "clearance" ) } );
}


BOOST_AUTO_TEST_CASE( SnapshotCheckMatchesDirectRun )
{
    std::shared_ptr<DRC_ENGINE> engine = m_board->GetDesignSettings().m_DRCEngine;
    LIVE_DRC_RESULTS            direct;

    engine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& )
            {
                if( LIVE_DRC::IsLiveViolation( aItem->GetErrorCode() ) )
                    direct.push_back( { std::make_unique<PCB_MARKER>( aItem, aPos, aLayer ), RPT_SEVERITY_ERROR } );
            } );

    engine->RunTests( EDA_UNITS::MM, false, false, nullptr, LIVE_DRC::Providers() );
    engine->ClearViolationHandler();

    size_t           markersBefore = m_board->Markers().size();
    LIVE_DRC_RESULTS live = LIVE_DRC::Check( LIVE_DRC::Snapshot( *m_board, EDA_UNITS::MM ), nullptr );

    BOOST_CHECK_EQUAL( m_board->Markers().size(), markersBefore );
    BOOST_REQUIRE( !live.empty() );
    BOOST_CHECK( describe( live ) == describe( direct ) );

    for( const LIVE_DRC_VIOLATION& violation : live )
    {
        DRC_ITEM* item = static_cast<DRC_ITEM*>( violation.marker->GetRCItem().get() );

        BOOST_CHECK( !violation.marker->GetBoard() );
        BOOST_CHECK( !item->GetViolatingRule() );
        BOOST_CHECK( !item->GetViolatingTest() );
        BOOST_CHECK_EQUAL( violation.severity, RPT_SEVERITY_ERROR );
    }
}


BOOST_AUTO_TEST_CASE( ConcurrentChecksAgree )
{
    LIVE_DRC_SNAPSHOT first = LIVE_DRC::Snapshot( *m_board, EDA_UNITS::MM );
    LIVE_DRC_SNAPSHOT second = LIVE_DRC::Snapshot( *m_board, EDA_UNITS::MM );
    LIVE_DRC_RESULTS  firstResults;
    LIVE_DRC_RESULTS  secondResults;

    {
        std::jthread a( [&]() { firstResults = LIVE_DRC::Check( std::move( first ), nullptr ); } );
        std::jthread b( [&]() { secondResults = LIVE_DRC::Check( std::move( second ), nullptr ); } );
    }

    BOOST_REQUIRE( !firstResults.empty() );
    BOOST_CHECK( describe( firstResults ) == describe( secondResults ) );
}


BOOST_AUTO_TEST_CASE( CancelledCheckReportsNothing )
{
    TEST_CANCEL_REPORTER reporter;
    reporter.m_cancelled = true;

    LIVE_DRC_RESULTS results = LIVE_DRC::Check( LIVE_DRC::Snapshot( *m_board, EDA_UNITS::MM ), &reporter );

    BOOST_CHECK( results.empty() );
}


BOOST_AUTO_TEST_CASE( ExcludedViolationsAreSkipped )
{
    LIVE_DRC_RESULTS all = LIVE_DRC::Check( LIVE_DRC::Snapshot( *m_board, EDA_UNITS::MM ), nullptr );
    BOOST_REQUIRE( !all.empty() );

    m_board->GetDesignSettings().m_DrcExclusions.insert( DRC_EXCLUSION::FromMarker( *all.front().marker ) );

    LIVE_DRC_RESULTS remaining = LIVE_DRC::Check( LIVE_DRC::Snapshot( *m_board, EDA_UNITS::MM ), nullptr );

    BOOST_CHECK_LT( remaining.size(), all.size() );
}


BOOST_AUTO_TEST_SUITE_END()
