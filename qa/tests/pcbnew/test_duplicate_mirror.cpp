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

#include <tools/edit_tool.h>


BOOST_AUTO_TEST_SUITE( DuplicateMirror )


BOOST_AUTO_TEST_CASE( SoicLeftRowContinuesCounterClockwise )
{
    // Pins 1..4 drawn down the left side, mirrored to the right
    std::map<wxString, wxString> numbers =
            EDIT_TOOL::MirroredPadNumbers( { "1", "2", "3", "4" }, { "1", "2", "3", "4" } );

    BOOST_REQUIRE_EQUAL( numbers.size(), 4 );
    BOOST_CHECK_EQUAL( numbers["1"], "8" );
    BOOST_CHECK_EQUAL( numbers["2"], "7" );
    BOOST_CHECK_EQUAL( numbers["3"], "6" );
    BOOST_CHECK_EQUAL( numbers["4"], "5" );
}


BOOST_AUTO_TEST_CASE( CopiesCountOnFromHighestPad )
{
    // An exposed pad numbered 9 is already there and not part of the mirrored row
    std::map<wxString, wxString> numbers =
            EDIT_TOOL::MirroredPadNumbers( { "2", "1" }, { "1", "2", "9" } );

    BOOST_REQUIRE_EQUAL( numbers.size(), 2 );
    BOOST_CHECK_EQUAL( numbers["1"], "11" );
    BOOST_CHECK_EQUAL( numbers["2"], "10" );
}


BOOST_AUTO_TEST_CASE( NonNumericAndEmptyNumbersAreKept )
{
    std::map<wxString, wxString> numbers =
            EDIT_TOOL::MirroredPadNumbers( { "A1", "", "3", "3" }, { "A1", "", "3" } );

    // Two pads sharing a number keep sharing it
    BOOST_REQUIRE_EQUAL( numbers.size(), 1 );
    BOOST_CHECK_EQUAL( numbers["3"], "4" );
    BOOST_CHECK( !numbers.count( "A1" ) );
    BOOST_CHECK( !numbers.count( "" ) );
}


BOOST_AUTO_TEST_SUITE_END()
