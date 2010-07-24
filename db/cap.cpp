// @file cap.cpp capped collection related 

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "../scripting/engine.h"
#include "btree.h"
#include <algorithm>
#include <list>
#include "query.h"
#include "queryutil.h"
#include "json.h"

namespace mongo {

    /* combine adjacent deleted records

       this is O(n^2) but we call it for capped tables where typically n==1 or 2!
       (or 3...there will be a little unused sliver at the end of the extent.)
    */
    void NamespaceDetails::compact() {
        assert(capped);

        list<DiskLoc> drecs;

        // Pull out capExtent's DRs from deletedList
        DiskLoc i = firstDeletedInCapExtent();
        for (; !i.isNull() && inCapExtent( i ); i = i.drec()->nextDeleted )
            drecs.push_back( i );
        firstDeletedInCapExtent() = i;

        // This is the O(n^2) part.
        drecs.sort();

        list<DiskLoc>::iterator j = drecs.begin();
        assert( j != drecs.end() );
        DiskLoc a = *j;
        while ( 1 ) {
            j++;
            if ( j == drecs.end() ) {
                DEBUGGING out() << "TEMP: compact adddelrec\n";
                addDeletedRec(a.drec(), a);
                break;
            }
            DiskLoc b = *j;
            while ( a.a() == b.a() && a.getOfs() + a.drec()->lengthWithHeaders == b.getOfs() ) {
                // a & b are adjacent.  merge.
                a.drec()->lengthWithHeaders += b.drec()->lengthWithHeaders;
                j++;
                if ( j == drecs.end() ) {
                    DEBUGGING out() << "temp: compact adddelrec2\n";
                    addDeletedRec(a.drec(), a);
                    return;
                }
                b = *j;
            }
            DEBUGGING out() << "temp: compact adddelrec3\n";
            addDeletedRec(a.drec(), a);
            a = b;
        }
    }

    void NamespaceDetails::cappedCheckMigrate() {
        // migrate old NamespaceDetails format
        assert( capped );
        if ( capExtent.a() == 0 && capExtent.getOfs() == 0 ) {
            capFirstNewRecord = DiskLoc();
            capFirstNewRecord.setInvalid();
            // put all the DeletedRecords in deletedList[ 0 ]
            for ( int i = 1; i < Buckets; ++i ) {
                DiskLoc first = deletedList[ i ];
                if ( first.isNull() )
                    continue;
                DiskLoc last = first;
                for (; !last.drec()->nextDeleted.isNull(); last = last.drec()->nextDeleted );
                last.drec()->nextDeleted = deletedList[ 0 ];
                deletedList[ 0 ] = first;
                deletedList[ i ] = DiskLoc();
            }
            // NOTE deletedList[ 1 ] set to DiskLoc() in above

            // Last, in case we're killed before getting here
            capExtent = firstExtent;
        }
    }

    bool NamespaceDetails::inCapExtent( const DiskLoc &dl ) const {
        assert( !dl.isNull() );
        // We could have a rec or drec, doesn't matter.
        return dl.drec()->myExtent( dl ) == capExtent.ext();
    }

    bool NamespaceDetails::nextIsInCapExtent( const DiskLoc &dl ) const {
        assert( !dl.isNull() );
        DiskLoc next = dl.drec()->nextDeleted;
        if ( next.isNull() )
            return false;
        return inCapExtent( next );
    }

    void NamespaceDetails::advanceCapExtent( const char *ns ) {
        // We want deletedList[ 1 ] to be the last DeletedRecord of the prev cap extent
        // (or DiskLoc() if new capExtent == firstExtent)
        if ( capExtent == lastExtent )
            deletedList[ 1 ] = DiskLoc();
        else {
            DiskLoc i = firstDeletedInCapExtent();
            for (; !i.isNull() && nextIsInCapExtent( i ); i = i.drec()->nextDeleted );
            deletedList[ 1 ] = i;
        }

        capExtent = theCapExtent()->xnext.isNull() ? firstExtent : theCapExtent()->xnext;

        /* this isn't true if a collection has been renamed...that is ok just used for diagnostics */
        //dassert( theCapExtent()->ns == ns );

        theCapExtent()->assertOk();
        capFirstNewRecord = DiskLoc();
    }

    DiskLoc NamespaceDetails::__capAlloc( int len ) {
        DiskLoc prev = deletedList[ 1 ];
        DiskLoc i = firstDeletedInCapExtent();
        DiskLoc ret;
        for (; !i.isNull() && inCapExtent( i ); prev = i, i = i.drec()->nextDeleted ) {
            // We need to keep at least one DR per extent in deletedList[ 0 ],
            // so make sure there's space to create a DR at the end.
            if ( i.drec()->lengthWithHeaders >= len + 24 ) {
                ret = i;
                break;
            }
        }

        /* unlink ourself from the deleted list */
        if ( !ret.isNull() ) {
            if ( prev.isNull() )
                deletedList[ 0 ] = ret.drec()->nextDeleted;
            else
                prev.drec()->nextDeleted = ret.drec()->nextDeleted;
            ret.drec()->nextDeleted.setInvalid(); // defensive.
            assert( ret.drec()->extentOfs < ret.getOfs() );
        }

        return ret;
    }

    DiskLoc NamespaceDetails::cappedAlloc(const char *ns, int len) { 
        // signal done allocating new extents.
        if ( !deletedList[ 1 ].isValid() )
            deletedList[ 1 ] = DiskLoc();
        
        assert( len < 400000000 );
        int passes = 0;
        int maxPasses = ( len / 30 ) + 2; // 30 is about the smallest entry that could go in the oplog
        if ( maxPasses < 5000 ){
            // this is for bacwards safety since 5000 was the old value
            maxPasses = 5000;
        }
        DiskLoc loc;

        // delete records until we have room and the max # objects limit achieved.

        /* this fails on a rename -- that is ok but must keep commented out */
        //assert( theCapExtent()->ns == ns );

        theCapExtent()->assertOk();
        DiskLoc firstEmptyExtent;
        while ( 1 ) {
            if ( nrecords < max ) {
                loc = __capAlloc( len );
                if ( !loc.isNull() )
                    break;
            }

            // If on first iteration through extents, don't delete anything.
            if ( !capFirstNewRecord.isValid() ) {
                advanceCapExtent( ns );
                if ( capExtent != firstExtent )
                    capFirstNewRecord.setInvalid();
                // else signal done with first iteration through extents.
                continue;
            }

            if ( !capFirstNewRecord.isNull() &&
                    theCapExtent()->firstRecord == capFirstNewRecord ) {
                // We've deleted all records that were allocated on the previous
                // iteration through this extent.
                advanceCapExtent( ns );
                continue;
            }

            if ( theCapExtent()->firstRecord.isNull() ) {
                if ( firstEmptyExtent.isNull() )
                    firstEmptyExtent = capExtent;
                advanceCapExtent( ns );
                if ( firstEmptyExtent == capExtent ) {
                    maybeComplain( ns, len );
                    return DiskLoc();
                }
                continue;
            }

            DiskLoc fr = theCapExtent()->firstRecord;
            theDataFileMgr.deleteRecord(ns, fr.rec(), fr, true); // ZZZZZZZZZZZZ
            compact();
            if( ++passes > maxPasses ) {
                log() << "passes ns:" << ns << " len:" << len << " maxPasses: " << maxPasses << '\n';
                log() << "passes max:" << max << " nrecords:" << nrecords << " datasize: " << datasize << endl;
                massert( 10345 ,  "passes >= maxPasses in capped collection alloc", false );
            }
        }

        // Remember first record allocated on this iteration through capExtent.
        if ( capFirstNewRecord.isValid() && capFirstNewRecord.isNull() )
            capFirstNewRecord = loc;

        return loc;
    }

    /* TODO : slow implementation to get us going.  fix later! */
    void cappedTruncateAfter(const char *ns, DiskLoc l) {
        NamespaceDetails *d = nsdetails(ns);
        /*
        ReverseCappedCursor c(d);
        while( c.ok() ) { 
            DiskLoc d = c.currLoc();
            if( d == l ) 
                break;
            c.advance();
        }
        */
    }

}
