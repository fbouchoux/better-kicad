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
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/**
 * @file
 * Regression test for issue #24391: entering a group could reach VIEW::Hide() with a null
 * VIEW_ITEM, which was dereferenced (viewPrivData() on a null this) and crashed. The public
 * mutators must tolerate a null item the same way they already tolerate an item with no view
 * data.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <view/view.h>
#include <view/view_group.h>

using namespace KIGFX;


BOOST_AUTO_TEST_SUITE( ViewNullSafety )


BOOST_AUTO_TEST_CASE( HideAndSetVisibleTolerateNullItem )
{
    VIEW view;

    // Before the fix each of these dereferenced the null item via viewPrivData() and aborted.
    view.Hide( nullptr, true );
    view.Hide( nullptr, false, true );
    view.SetVisible( nullptr, true );
    view.SetVisible( nullptr, false );

    // Reaching this point without crashing is the assertion.
    BOOST_TEST( true );
}


BOOST_AUTO_TEST_CASE( ViewGroupRejectsNullItem )
{
    // Issue #24778: a re-entrant ExitGroup() during SCH_SELECTION_TOOL::EnterGroup() left the
    // group overlay with a null (stale) member. VIEW_GROUP stored it, then the next repaint
    // dereferenced it in ViewBBox()/ViewDraw(). Add() must drop a null the same way the VIEW
    // mutators do.
    VIEW_GROUP group;

    group.Add( nullptr );

    BOOST_TEST( group.GetSize() == 0u );

    // Without the guard this iterated m_groupItems[0] on a null item and crashed.
    group.ViewBBox();
}


/**
 * Diagnostic queries may include display-only overlays without making hidden layers hittable.
 */
BOOST_AUTO_TEST_CASE( LayerQueryCanOptIntoDisplayOnlyOverlays )
{
    // Empty groups cover the view, making both candidates overlap the query rectangle
    VIEW view;
    VIEW_GROUP first;
    VIEW_GROUP second;
    constexpr int layer = 0;
    first.SetLayer( layer );
    second.SetLayer( layer );
    view.Add( &first );
    view.Add( &second );
    view.SetLayerDisplayOnly( layer );

    const BOX2I bounds( VECTOR2I( 0, 0 ), VECTOR2I( 10, 10 ) );
    int count = 0;
    auto countItem = [&]( VIEW_ITEM* )
    {
        ++count;
        return true;
    };

    // Ordinary hit testing must continue to ignore overlays
    view.Query( layer, bounds, countItem );
    BOOST_CHECK_EQUAL( count, 0 );

    // Diagnostic hit testing explicitly includes the overlay's spatial index
    view.Query( layer, bounds, countItem, true );
    BOOST_CHECK_EQUAL( count, 2 );

    // A tooltip can stop searching after its first hit
    count = 0;
    view.Query( layer, bounds,
                [&]( VIEW_ITEM* )
                {
                    ++count;
                    return false;
                },
                true );
    BOOST_CHECK_EQUAL( count, 1 );

    // The opt-in changes selectability filtering, not layer visibility
    count = 0;
    view.SetLayerVisible( layer, false );
    view.Query( layer, bounds, countItem, true );
    BOOST_CHECK_EQUAL( count, 0 );
}


BOOST_AUTO_TEST_SUITE_END()
