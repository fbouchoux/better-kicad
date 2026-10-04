/* Temporary, unconditional interactive-router performance trace. */

#include "pns_performance_trace.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

namespace
{
struct MOVE_STATS
{
    uint64_t id = 0;
    uint64_t clearanceCalls = 0;
    uint64_t boardCacheHits = 0;
    uint64_t temporaryCacheHits = 0;
    uint64_t clearanceMisses = 0;
    uint64_t clearanceUs = 0;
    uint64_t constraintCalls = 0;
    uint64_t evalCalls = 0;
    uint64_t segmentEvals = 0;
    uint64_t constraintUs = 0;
};

std::mutex traceMutex;
FILE* traceFile = nullptr;
uint64_t nextMove = 0;
MOVE_STATS stats;

uint64_t elapsedUs( PNS::PERFORMANCE_TRACE::CLOCK::time_point aStart )
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
            PNS::PERFORMANCE_TRACE::CLOCK::now() - aStart ).count();
}

void openTrace()
{
    if( traceFile )
        return;

    wxString logName = wxString::Format( wxS( "kicad-pns-performance-%lu.log" ),
                                         static_cast<unsigned long>( wxGetProcessId() ) );
    wxFileName executable( wxStandardPaths::Get().GetExecutablePath() );
    wxFileName path( executable.GetPath(), logName );
    traceFile = wxFopen( path.GetFullPath(), wxS( "wb" ) );

    if( !traceFile )
    {
        fprintf( stderr, "Unable to open PNS performance trace at %s\n",
                 static_cast<const char*>( path.GetFullPath().utf8_str() ) );
        path.Assign( wxFileName::GetTempDir(), logName );
        traceFile = wxFopen( path.GetFullPath(), wxS( "wb" ) );
    }

    if( traceFile )
    {
        fprintf( traceFile, "# Path: %s\n",
                 static_cast<const char*>( path.GetFullPath().utf8_str() ) );
        fprintf( traceFile, "# Temporary KiCad PNS performance trace (times in us)\n" );
        fprintf( traceFile, "# CONSTRAINT,move,type,eval_calls,segment_evals,total_us,rule,"
                            "layer,kind_a,kind_b,parent_type_a,parent_type_b\n" );
        fprintf( traceFile, "# MOVE,id,result,total_us,clearance_calls,board_hits,temp_hits,"
                            "misses,clearance_us,constraint_calls,eval_calls,segment_evals,constraint_us\n" );
        fflush( traceFile );
    }
    else
    {
        fprintf( stderr, "Unable to open fallback PNS performance trace at %s\n",
                 static_cast<const char*>( path.GetFullPath().utf8_str() ) );
    }
}

wxString csvEscape( wxString aText )
{
    aText.Replace( wxS( "\"" ), wxS( "\"\"" ) );
    return aText;
}
} // namespace


uint64_t PNS::PERFORMANCE_TRACE::BeginMove( const VECTOR2I& aPosition, int aState )
{
    std::lock_guard<std::mutex> lock( traceMutex );
    openTrace();

    stats = MOVE_STATS();
    stats.id = ++nextMove;

    if( traceFile )
    {
        fprintf( traceFile, "BEGIN,%llu,%d,%d,%d\n",
                 static_cast<unsigned long long>( stats.id ), aPosition.x, aPosition.y, aState );
        fflush( traceFile );
    }

    return stats.id;
}


void PNS::PERFORMANCE_TRACE::EndMove( uint64_t aMove, CLOCK::time_point aStart, bool aResult )
{
    std::lock_guard<std::mutex> lock( traceMutex );

    if( !traceFile || stats.id != aMove )
        return;

    fprintf( traceFile, "MOVE,%llu,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
             static_cast<unsigned long long>( aMove ), aResult,
             static_cast<unsigned long long>( elapsedUs( aStart ) ),
             static_cast<unsigned long long>( stats.clearanceCalls ),
             static_cast<unsigned long long>( stats.boardCacheHits ),
             static_cast<unsigned long long>( stats.temporaryCacheHits ),
             static_cast<unsigned long long>( stats.clearanceMisses ),
             static_cast<unsigned long long>( stats.clearanceUs ),
             static_cast<unsigned long long>( stats.constraintCalls ),
             static_cast<unsigned long long>( stats.evalCalls ),
             static_cast<unsigned long long>( stats.segmentEvals ),
             static_cast<unsigned long long>( stats.constraintUs ) );
    fflush( traceFile );
    stats.id = 0;
}


void PNS::PERFORMANCE_TRACE::RecordClearance( const char* aCache, CLOCK::time_point aStart )
{
    std::lock_guard<std::mutex> lock( traceMutex );

    if( !stats.id )
        return;

    stats.clearanceCalls++;
    stats.clearanceUs += elapsedUs( aStart );

    if( strcmp( aCache, "board" ) == 0 )
        stats.boardCacheHits++;
    else if( strcmp( aCache, "temporary" ) == 0 )
        stats.temporaryCacheHits++;
    else
        stats.clearanceMisses++;
}


void PNS::PERFORMANCE_TRACE::RecordConstraint( int aType, int aEvalCalls, int aSegmentEvals,
                                               CLOCK::time_point aStart, const wxString& aRule,
                                               int aLayer, int aKindA, int aKindB,
                                               int aParentTypeA, int aParentTypeB )
{
    std::lock_guard<std::mutex> lock( traceMutex );

    if( !stats.id || !traceFile )
        return;

    uint64_t duration = elapsedUs( aStart );
    stats.constraintCalls++;
    stats.evalCalls += aEvalCalls;
    stats.segmentEvals += aSegmentEvals;
    stats.constraintUs += duration;

    std::string rule = csvEscape( aRule ).ToStdString( wxConvUTF8 );
    fprintf( traceFile, "CONSTRAINT,%llu,%d,%d,%d,%llu,\"%s\",%d,%d,%d,%d,%d\n",
             static_cast<unsigned long long>( stats.id ), aType, aEvalCalls, aSegmentEvals,
             static_cast<unsigned long long>( duration ), rule.c_str(), aLayer, aKindA, aKindB,
             aParentTypeA, aParentTypeB );
    fflush( traceFile );
}


void PNS::PERFORMANCE_TRACE::RecordPhase( const char* aName, CLOCK::time_point aStart,
                                          int64_t aValue1, int64_t aValue2 )
{
    std::lock_guard<std::mutex> lock( traceMutex );

    if( !stats.id || !traceFile )
        return;

    fprintf( traceFile, "PHASE,%llu,%s,%llu,%lld,%lld\n",
             static_cast<unsigned long long>( stats.id ), aName,
             static_cast<unsigned long long>( elapsedUs( aStart ) ),
             static_cast<long long>( aValue1 ), static_cast<long long>( aValue2 ) );
    fflush( traceFile );
}


void PNS::PERFORMANCE_TRACE::RecordWalkObstacle( int aIteration, int aPolicy, const void* aItem,
                                                 int aKind, int aParentType, int aClearance,
                                                 int aClusterSize, int aLineSegments,
                                                 const VECTOR2I& aPosition )
{
    std::lock_guard<std::mutex> lock( traceMutex );

    if( !stats.id || !traceFile )
        return;

    fprintf( traceFile, "WALK,%llu,%d,%d,%p,%d,%d,%d,%d,%d,%d,%d\n",
             static_cast<unsigned long long>( stats.id ), aIteration, aPolicy, aItem, aKind,
             aParentType, aClearance, aClusterSize, aLineSegments, aPosition.x, aPosition.y );
    fflush( traceFile );
}
