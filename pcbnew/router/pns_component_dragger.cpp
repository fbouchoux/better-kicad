/*
 * KiRouter - a push-and-(sometimes-)shove PCB router
 *
 * Copyright (C) 2013-2020 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <memory>

#include "pns_arc.h"
#include "pns_line.h"
#include "pns_solid.h"
#include "pns_via.h"
#include "pns_router.h"

#include "pns_component_dragger.h"
#include "pns_debug_decorator.h"

namespace PNS
{

COMPONENT_DRAGGER::COMPONENT_DRAGGER( ROUTER* aRouter ) : DRAG_ALGO( aRouter )
{
    // ensure all variables are initialized
    m_dragStatus = false;
    m_currentNode = nullptr;
}


COMPONENT_DRAGGER::~COMPONENT_DRAGGER()
{
}


bool COMPONENT_DRAGGER::Start( const VECTOR2I& aP, ITEM_SET& aPrimitives )
{
    assert( m_world );

    m_currentNode         = nullptr;
    m_initialDraggedItems = aPrimitives;
    m_p0                  = aP;

    std::unordered_set<LINKED_ITEM*> seenItems;

    auto anchorPosition =
            []( ITEM* aItem )
            {
                if( aItem->Kind() == ITEM::SOLID_T )
                    return static_cast<SOLID*>( aItem )->Pos();

                return static_cast<VIA*>( aItem )->Pos();
            };

    auto addLinked =
            [&]( ITEM* aAnchor, const JOINT* aJoint, LINKED_ITEM* aItem, VECTOR2I aOffset = {} )
            {
                if( seenItems.count( aItem ) )
                    return;

                seenItems.insert( aItem );

                // Segments that go directly between two linked anchors are special-cased
                VECTOR2I otherEnd = ( aJoint->Pos() == aItem->Anchor( 0 ) ) ? aItem->Anchor( 1 )
                                                                            : aItem->Anchor( 0 );
                const JOINT* otherJoint = m_world->FindJoint( otherEnd, aItem->Layer(), aItem->Net() );

                if( otherJoint && otherJoint->LinkCount( ITEM::SOLID_T | ITEM::VIA_T ) )
                {
                    for( ITEM* otherItem : otherJoint->LinkList() )
                    {
                        if( aPrimitives.Contains( otherItem ) )
                        {
                            m_fixedItems.insert( aItem );
                            return;
                        }
                    }
                }

                int segIndex;
                DRAGGED_CONNECTION cn;

                cn.origLine     = m_world->AssembleLine( aItem, &segIndex );
                cn.attachedItem = aAnchor;
                cn.offset       = aOffset;

                // Lines that go directly between two linked anchors are also special-cased
                const SHAPE_LINE_CHAIN& line = cn.origLine.CLine();
                const JOINT* jA = m_world->FindJoint( line.CPoint( 0 ), aItem->Layer(), aItem->Net() );
                const JOINT* jB = m_world->FindJoint( line.CLastPoint(), aItem->Layer(), aItem->Net() );

                wxASSERT( jA == aJoint || jB == aJoint );
                const JOINT* jSearch = ( jA == aJoint ) ? jB : jA;

                if( jSearch && jSearch->LinkCount( ITEM::SOLID_T | ITEM::VIA_T ) )
                {
                    for( ITEM* otherItem : jSearch->LinkList() )
                    {
                        if( aPrimitives.Contains( otherItem ) )
                        {
                            for( ITEM* item : cn.origLine.Links() )
                                m_fixedItems.insert( item );

                            return;
                        }
                    }
                }

                m_conns.push_back( std::move( cn ) );
            };

    for( ITEM* item : aPrimitives.Items() )
    {
        if( !item->OfKind( ITEM::SOLID_T | ITEM::VIA_T ) )
            continue;

        item->Unmark( MK_LOCKED );
        m_anchors.insert( item );

        if( !item->IsRoutable() )
            continue;

        VECTOR2I position = anchorPosition( item );
        std::set<const JOINT*> joints;

        if( item->Kind() == ITEM::SOLID_T )
        {
            if( const JOINT* joint = m_world->FindJoint( position, item ) )
                joints.insert( joint );
        }
        else
        {
            for( int layer = item->Layers().Start(); layer <= item->Layers().End(); ++layer )
            {
                if( const JOINT* joint = m_world->FindJoint( position, layer, item->Net() ) )
                    joints.insert( joint );
            }
        }

        for( const JOINT* joint : joints )
        {
            for( ITEM* link : joint->LinkList() )
            {
                if( link->OfKind( ITEM::SEGMENT_T | ITEM::ARC_T ) )
                    addLinked( item, joint, static_cast<LINKED_ITEM*>( link ) );
            }
        }

        // Pads may contain a dangling track endpoint away from their anchor point.
        if( item->Kind() == ITEM::SOLID_T )
        {
            SOLID* solid = static_cast<SOLID*>( item );
            std::vector<JOINT*> extraJoints;

            m_world->QueryJoints( solid->Hull().BBox(), extraJoints, solid->Layers(),
                                  ITEM::SEGMENT_T | ITEM::ARC_T );

            for( JOINT* extraJoint : extraJoints )
            {
                if( extraJoint->Net() == solid->Net() && extraJoint->LinkCount() == 1 )
                {
                    LINKED_ITEM* li = static_cast<LINKED_ITEM*>( extraJoint->LinkList().front() );

                    if( li->Collide( solid, m_world, solid->Layer() ) )
                        addLinked( solid, extraJoint, li, extraJoint->Pos() - solid->Pos() );
                }
            }
        }
    }

    return true;
}


bool COMPONENT_DRAGGER::Drag( const VECTOR2I& aP )
{
    assert( m_world );

    auto anchorPosition =
            []( ITEM* aItem )
            {
                if( aItem->Kind() == ITEM::SOLID_T )
                    return static_cast<SOLID*>( aItem )->Pos();

                return static_cast<VIA*>( aItem )->Pos();
            };

    m_world->KillChildren();
    m_currentNode = m_world->Branch();

    for( ITEM* item : m_initialDraggedItems )
        m_currentNode->Remove( item );

    m_draggedItems.Clear();

    for( ITEM* anchor : m_anchors )
    {
        VECTOR2I p_next;

        if( anchor->Kind() == ITEM::SOLID_T )
        {
            SOLID* solid = static_cast<SOLID*>( anchor );
            p_next = aP - m_p0 + solid->Pos();

            std::unique_ptr<SOLID> moved( static_cast<SOLID*>( solid->Clone() ) );
            moved->SetPos( p_next );
            m_draggedItems.Add( moved.get() );
            m_currentNode->Add( std::move( moved ) );
        }
        else
        {
            VIA* via = static_cast<VIA*>( anchor );
            p_next = aP - m_p0 + via->Pos();

            std::unique_ptr<VIA> moved( via->Clone() );
            moved->SetPos( p_next );
            m_draggedItems.Add( moved.get() );
            m_currentNode->Add( std::move( moved ) );
        }

        if( !anchor->IsRoutable() )
            continue;

        for( DRAGGED_CONNECTION& l : m_conns )
        {
            if( l.attachedItem == anchor )
            {
                l.p_orig = anchorPosition( anchor ) + l.offset;
                l.p_next = p_next + l.offset;
            }
        }
    }

    for( ITEM* item : m_fixedItems )
    {
        m_currentNode->Remove( item );

        switch( item->Kind() )
        {
        case ITEM::SEGMENT_T:
        {
            SEGMENT*                 s = static_cast<SEGMENT*>( item );
            std::unique_ptr<SEGMENT> s_new( s->Clone() );

            SEG orig = s->Seg();
            s_new->SetEnds( aP - m_p0 + orig.A, aP - m_p0 + orig.B );

            m_draggedItems.Add( s_new.get() );
            m_currentNode->Add( std::move( s_new ) );

            break;
        }

        case ITEM::ARC_T:
        {
            ARC* a = static_cast<ARC*>( item );
            std::unique_ptr<ARC> a_new( a->Clone() );

            SHAPE_ARC& arc = a_new->Arc();
            arc.Move( aP - m_p0 );

            m_draggedItems.Add( a_new.get() );
            m_currentNode->Add( std::move( a_new ) );
            break;
        }

        default:
            wxFAIL_MSG( wxT( "Unexpected item type in COMPONENT_DRAGGER::m_fixedItems" ) );
        }
    }

    for( COMPONENT_DRAGGER::DRAGGED_CONNECTION& cn : m_conns )
    {
        LINE l_new( cn.origLine );
        l_new.Unmark();
        l_new.ClearLinks();
        l_new.DragCorner( cn.p_next, cn.origLine.CLine().Find( cn.p_orig ) );

        PNS_DBG( Dbg(), AddItem, &l_new, BLUE, 0, wxT( "cdrag-new-fanout" ) );
        m_draggedItems.Add( l_new );

        LINE l_orig( cn.origLine );
        m_currentNode->Remove( l_orig );
        m_currentNode->Add( l_new );
    }

    return true;
}


bool COMPONENT_DRAGGER::FixRoute( bool aForceCommit )
{
    NODE* node = CurrentNode();

    if( node )
    {
        if( Settings().AllowDRCViolations() || aForceCommit || !node->CheckColliding( m_draggedItems ) )
        {
            Router()->CommitRouting( node );
            return true;
        }
    }

    return false;
}


NODE* COMPONENT_DRAGGER::CurrentNode() const
{
    return m_currentNode ? m_currentNode : m_world;
}


const ITEM_SET COMPONENT_DRAGGER::Traces()
{
    return m_draggedItems;
}

}; // namespace PNS
