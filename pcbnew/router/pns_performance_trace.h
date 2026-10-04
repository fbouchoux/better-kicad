/* Temporary, unconditional interactive-router performance trace. */

#ifndef PNS_PERFORMANCE_TRACE_H
#define PNS_PERFORMANCE_TRACE_H

#include <chrono>
#include <cstdint>

#include <math/vector2d.h>
#include <wx/string.h>

namespace PNS
{
class PERFORMANCE_TRACE
{
public:
    using CLOCK = std::chrono::steady_clock;

    static uint64_t BeginMove( const VECTOR2I& aPosition, int aState );
    static void EndMove( uint64_t aMove, CLOCK::time_point aStart, bool aResult );
    static void RecordClearance( const char* aCache, CLOCK::time_point aStart );
    static void RecordConstraint( int aType, int aEvalCalls, int aSegmentEvals,
                                  CLOCK::time_point aStart, const wxString& aRule, int aLayer,
                                  int aKindA, int aKindB, int aParentTypeA, int aParentTypeB );
    static void RecordPhase( const char* aName, CLOCK::time_point aStart,
                             int64_t aValue1 = 0, int64_t aValue2 = 0 );
    static void RecordWalkObstacle( int aIteration, int aPolicy, const void* aItem, int aKind,
                                    int aParentType, int aClearance, int aClusterSize,
                                    int aLineSegments, const VECTOR2I& aPosition );
};
} // namespace PNS

#endif
