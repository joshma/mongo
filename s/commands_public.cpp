// s/commands_public.cpp


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
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"
#include "../client/parallel.h"
#include "../db/commands.h"
#include "../db/query.h"

#include "config.h"
#include "chunk.h"
#include "strategy.h"
#include "grid.h"

namespace mongo {

    namespace dbgrid_pub_cmds {

        class PublicGridCommand : public Command {
        public:
            PublicGridCommand( const char* n, const char* oldname=NULL ) : Command( n, false, oldname ) {
            }
            virtual bool slaveOk() const {
                return true;
            }
            virtual bool adminOnly() const {
                return false;
            }

            // all grid commands are designed not to lock
            virtual LockType locktype() const { return NONE; }

        protected:
            bool passthrough( DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ) {
                return _passthrough(conf->getName(), conf, cmdObj, result);
            }
            bool adminPassthrough( DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ) {
                return _passthrough("admin", conf, cmdObj, result);
            }

        private:
            bool _passthrough(const string& db,  DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ) {
                ShardConnection conn( conf->getPrimary() , "" );
                BSONObj res;
                bool ok = conn->runCommand( db , cmdObj , res );
                result.appendElements( res );
                conn.done();
                return ok;
            }
        };

        class RunOnAllShardsCommand : public Command {
        public:
            RunOnAllShardsCommand(const char* n, const char* oldname=NULL) : Command(n, false, oldname) {}

            virtual bool slaveOk() const { return true; }
            virtual bool adminOnly() const { return false; }

            // all grid commands are designed not to lock
            virtual LockType locktype() const { return NONE; }


            // default impl uses all shards for DB
            virtual void getShards(const string& dbName , BSONObj& cmdObj, set<Shard>& shards) {
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                conf->getAllShards(shards);
            }

            virtual void aggregateResults(const vector<BSONObj>& results, BSONObjBuilder& output) {}

            // don't override
            virtual bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& output, bool) {
                set<Shard> shards;
                getShards(dbName, cmdObj, shards);

                list< shared_ptr<Future::CommandResult> > futures;
                for ( set<Shard>::const_iterator i=shards.begin(), end=shards.end() ; i != end ; i++ ) {
                    futures.push_back( Future::spawnCommand( i->getConnString() , dbName , cmdObj ) );
                }

                vector<BSONObj> results;
                BSONObjBuilder subobj (output.subobjStart("raw"));
                BSONObjBuilder errors;
                for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ) {
                    shared_ptr<Future::CommandResult> res = *i;
                    if ( ! res->join() ) {
                        errors.appendAs(res->result()["errmsg"], res->getServer());
                    }
                    results.push_back( res->result() );
                    subobj.append( res->getServer() , res->result() );
                }

                subobj.done();

                BSONObj errobj = errors.done();
                if (! errobj.isEmpty()) {
                    errmsg = errobj.toString(false, true);
                    return false;
                }

                aggregateResults(results, output);
                return true;
            }

        };

        class AllShardsCollectionCommand : public RunOnAllShardsCommand {
        public:
            AllShardsCollectionCommand(const char* n, const char* oldname=NULL) : RunOnAllShardsCommand(n, oldname) {}

            virtual void getShards(const string& dbName , BSONObj& cmdObj, set<Shard>& shards) {
                string fullns = dbName + '.' + cmdObj.firstElement().valuestrsafe();

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    shards.insert(conf->getShard(fullns));
                }
                else {
                    conf->getChunkManager(fullns)->getAllShards(shards);
                }
            }
        };


        class NotAllowedOnShardedCollectionCmd : public PublicGridCommand {
        public:
            NotAllowedOnShardedCollectionCmd( const char * n ) : PublicGridCommand( n ) {}

            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) = 0;

            virtual bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string fullns = getFullNS( dbName , cmdObj );

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result );
                }
                errmsg = "can't do command: " + name + " on sharded collection";
                return false;
            }
        };

        // ----

        class DropIndexesCmd : public AllShardsCollectionCommand {
        public:
            DropIndexesCmd() :  AllShardsCollectionCommand("dropIndexes", "deleteIndexes") {}
        } dropIndexesCmd;

        class ReIndexCmd : public AllShardsCollectionCommand {
        public:
            ReIndexCmd() :  AllShardsCollectionCommand("reIndex") {}
        } reIndexCmd;

        class ValidateCmd : public AllShardsCollectionCommand {
        public:
            ValidateCmd() :  AllShardsCollectionCommand("validate") {}
        } validateCmd;

        class RepairDatabaseCmd : public RunOnAllShardsCommand {
        public:
            RepairDatabaseCmd() :  RunOnAllShardsCommand("repairDatabase") {}
        } repairDatabaseCmd;

        class DBStatsCmd : public RunOnAllShardsCommand {
        public:
            DBStatsCmd() :  RunOnAllShardsCommand("dbStats", "dbstats") {}

            virtual void aggregateResults(const vector<BSONObj>& results, BSONObjBuilder& output) {
                long long objects = 0;
                long long dataSize = 0;
                long long storageSize = 0;
                long long numExtents = 0;
                long long indexes = 0;
                long long indexSize = 0;
                long long fileSize = 0;

                for (vector<BSONObj>::const_iterator it(results.begin()), end(results.end()); it != end; ++it) {
                    const BSONObj& b = *it;
                    objects     += b["objects"].numberLong();
                    dataSize    += b["dataSize"].numberLong();
                    storageSize += b["storageSize"].numberLong();
                    numExtents  += b["numExtents"].numberLong();
                    indexes     += b["indexes"].numberLong();
                    indexSize   += b["indexSize"].numberLong();
                    fileSize    += b["fileSize"].numberLong();
                }

                //result.appendNumber( "collections" , ncollections ); //TODO: need to find a good way to get this
                output.appendNumber( "objects" , objects );
                output.append      ( "avgObjSize" , double(dataSize) / double(objects) );
                output.appendNumber( "dataSize" , dataSize );
                output.appendNumber( "storageSize" , storageSize);
                output.appendNumber( "numExtents" , numExtents );
                output.appendNumber( "indexes" , indexes );
                output.appendNumber( "indexSize" , indexSize );
                output.appendNumber( "fileSize" , fileSize );
            }
        } DBStatsCmdObj;

        class DropCmd : public PublicGridCommand {
        public:
            DropCmd() : PublicGridCommand( "drop" ) {}
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                log() << "DROP: " << fullns << endl;

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 10418 ,  "how could chunk manager be null!" , cm );

                cm->drop( cm );
                uassert( 13512 , "drop collection attempted on non-sharded collection" , conf->removeSharding( fullns ) );

                return 1;
            }
        } dropCmd;

        class DropDBCmd : public PublicGridCommand {
        public:
            DropDBCmd() : PublicGridCommand( "dropDatabase" ) {}
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {

                BSONElement e = cmdObj.firstElement();

                if ( ! e.isNumber() || e.number() != 1 ) {
                    errmsg = "invalid params";
                    return 0;
                }

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                log() << "DROP DATABASE: " << dbName << endl;

                if ( ! conf ) {
                    result.append( "info" , "database didn't exist" );
                    return true;
                }

                if ( ! conf->dropDatabase( errmsg ) )
                    return false;

                result.append( "dropped" , dbName );
                return true;
            }
        } dropDBCmd;

        class RenameCollectionCmd : public PublicGridCommand {
        public:
            RenameCollectionCmd() : PublicGridCommand( "renameCollection" ) {}
            bool run(const string& dbName, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string fullnsFrom = cmdObj.firstElement().valuestrsafe();
                string dbNameFrom = nsToDatabase( fullnsFrom.c_str() );
                DBConfigPtr confFrom = grid.getDBConfig( dbNameFrom , false );

                string fullnsTo = cmdObj["to"].valuestrsafe();
                string dbNameTo = nsToDatabase( fullnsTo.c_str() );
                DBConfigPtr confTo = grid.getDBConfig( dbNameTo , false );

                uassert(13140, "Don't recognize source or target DB", confFrom && confTo);
                uassert(13138, "You can't rename a sharded collection", !confFrom->isSharded(fullnsFrom));
                uassert(13139, "You can't rename to a sharded collection", !confTo->isSharded(fullnsTo));

                const Shard& shardTo = confTo->getShard(fullnsTo);
                const Shard& shardFrom = confFrom->getShard(fullnsFrom);

                uassert(13137, "Source and destination collections must be on same shard", shardFrom == shardTo);

                return adminPassthrough( confFrom , cmdObj , result );
            }
        } renameCollectionCmd;

        class CopyDBCmd : public PublicGridCommand {
        public:
            CopyDBCmd() : PublicGridCommand( "copydb" ) {}
            bool run(const string& dbName, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string todb = cmdObj.getStringField("todb");
                uassert(13402, "need a todb argument", !todb.empty());

                DBConfigPtr confTo = grid.getDBConfig( todb );
                uassert(13398, "cant copy to sharded DB", !confTo->isShardingEnabled());

                string fromhost = cmdObj.getStringField("fromhost");
                if (!fromhost.empty()) {
                    return adminPassthrough( confTo , cmdObj , result );
                }
                else {
                    string fromdb = cmdObj.getStringField("fromdb");
                    uassert(13399, "need a fromdb argument", !fromdb.empty());

                    DBConfigPtr confFrom = grid.getDBConfig( fromdb , false );
                    uassert(13400, "don't know where source DB is", confFrom);
                    uassert(13401, "cant copy from sharded DB", !confFrom->isShardingEnabled());

                    BSONObjBuilder b;
                    BSONForEach(e, cmdObj) {
                        if (strcmp(e.fieldName(), "fromhost") != 0)
                            b.append(e);
                    }
                    b.append("fromhost", confFrom->getPrimary().getConnString());
                    BSONObj fixed = b.obj();

                    return adminPassthrough( confTo , fixed , result );
                }

            }
        } copyDBCmd;

        class CountCmd : public PublicGridCommand {
        public:
            CountCmd() : PublicGridCommand("count") { }
            bool run(const string& dbName, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool l) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                BSONObj filter;
                if ( cmdObj["query"].isABSONObj() )
                    filter = cmdObj["query"].Obj();

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    ShardConnection conn( conf->getPrimary() , fullns );

                    BSONObj temp;
                    bool ok = conn->runCommand( dbName , cmdObj , temp );
                    conn.done();

                    if ( ok ) {
                        result.append( temp["n"] );
                        return true;
                    }

                    if ( temp["code"].numberInt() != StaleConfigInContextCode ) {
                        errmsg = temp["errmsg"].String();
                        result.appendElements( temp );
                        return false;
                    }

                    // this collection got sharded
                    ChunkManagerPtr cm = conf->getChunkManager( fullns , true );
                    if ( ! cm ) {
                        errmsg = "should be sharded now";
                        result.append( "root" , temp );
                        return false;
                    }
                }

                long long total = 0;
                map<string,long long> shardCounts;
                int numTries = 0;
                bool hadToBreak = false;

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                while ( numTries < 5 ) {
                    numTries++;

                    if ( ! cm ) {
                        // probably unsharded now
                        return run( dbName , cmdObj , errmsg , result , l );
                    }

                    set<Shard> shards;
                    cm->getShardsForQuery( shards , filter );
                    assert( shards.size() );

                    hadToBreak = false;

                    for (set<Shard>::iterator it=shards.begin(), end=shards.end(); it != end; ++it) {
                        ShardConnection conn(*it, fullns);
                        if ( conn.setVersion() ) {
                            total = 0;
                            shardCounts.clear();
                            cm = conf->getChunkManager( fullns );
                            conn.done();
                            hadToBreak = true;
                            break;
                        }

                        BSONObj temp;
                        bool ok = conn->runCommand( dbName , BSON( "count" << collection << "query" << filter ) , temp );
                        conn.done();

                        if ( ok ) {
                            long long mine = temp["n"].numberLong();
                            total += mine;
                            shardCounts[it->getName()] = mine;
                            continue;
                        }

                        if ( StaleConfigInContextCode == temp["code"].numberInt() ) {
                            // my version is old
                            total = 0;
                            shardCounts.clear();
                            cm = conf->getChunkManager( fullns , true );
                            hadToBreak = true;
                            break;
                        }

                        // command failed :(
                        errmsg = "failed on : " + it->getName();
                        result.append( "cause" , temp );
                        return false;
                    }
                    if ( ! hadToBreak )
                        break;
                }
                if (hadToBreak) {
                    errmsg = "Tried 5 times without success to get count for " + fullns + " from all shards";
                    return false;
                }

                total = applySkipLimit( total , cmdObj );
                result.appendNumber( "n" , total );
                BSONObjBuilder temp( result.subobjStart( "shards" ) );
                for ( map<string,long long>::iterator i=shardCounts.begin(); i!=shardCounts.end(); ++i )
                    temp.appendNumber( i->first , i->second );
                temp.done();
                return true;
            }
        } countCmd;

        class CollectionStats : public PublicGridCommand {
        public:
            CollectionStats() : PublicGridCommand("collStats", "collstats") { }
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    result.append( "ns" , fullns );
                    result.appendBool("sharded", false);
                    result.append( "primary" , conf->getPrimary().getName() );
                    return passthrough( conf , cmdObj , result);
                }
                result.appendBool("sharded", true);

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 12594 ,  "how could chunk manager be null!" , cm );

                set<Shard> servers;
                cm->getAllShards(servers);

                BSONObjBuilder shardStats;
                long long count=0;
                long long size=0;
                long long storageSize=0;
                int nindexes=0;
                bool warnedAboutIndexes = false;
                for ( set<Shard>::iterator i=servers.begin(); i!=servers.end(); i++ ) {
                    ScopedDbConnection conn( *i );
                    BSONObj res;
                    if ( ! conn->runCommand( dbName , cmdObj , res ) ) {
                        errmsg = "failed on shard: " + res.toString();
                        return false;
                    }
                    conn.done();

                    count += res["count"].numberLong();
                    size += res["size"].numberLong();
                    storageSize += res["storageSize"].numberLong();

                    int myIndexes = res["nindexes"].numberInt();

                    if ( nindexes == 0 ) {
                        nindexes = myIndexes;
                    }
                    else if ( nindexes == myIndexes ) {
                        // no-op
                    }
                    else {
                        // hopefully this means we're building an index

                        if ( myIndexes > nindexes )
                            nindexes = myIndexes;

                        if ( ! warnedAboutIndexes ) {
                            result.append( "warning" , "indexes don't all match - ok if ensureIndex is running" );
                            warnedAboutIndexes = true;
                        }
                    }

                    shardStats.append(i->getName(), res);
                }

                result.append("ns", fullns);
                result.appendNumber("count", count);
                result.appendNumber("size", size);
                result.append      ("avgObjSize", double(size) / double(count));
                result.appendNumber("storageSize", storageSize);
                result.append("nindexes", nindexes);

                result.append("nchunks", cm->numChunks());
                result.append("shards", shardStats.obj());

                return true;
            }
        } collectionStatsCmd;

        class FindAndModifyCmd : public PublicGridCommand {
        public:
            FindAndModifyCmd() : PublicGridCommand("findAndModify", "findandmodify") { }
            bool run(const string& dbName, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result);
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13002 ,  "how could chunk manager be null!" , cm );

                BSONObj filter = cmdObj.getObjectField("query");
                uassert(13343,  "query for sharded findAndModify must have shardkey", cm->hasShardKey(filter));

                //TODO with upsert consider tracking for splits

                ChunkPtr chunk = cm->findChunk(filter);
                ShardConnection conn( chunk->getShard() , fullns );
                BSONObj res;
                bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                conn.done();

                if (!ok && res.getIntField("code") == 9996) { // code for StaleConfigException
                    throw StaleConfigException(fullns, "FindAndModify"); // Command code traps this and re-runs
                }

                result.appendElements(res);
                return ok;
            }

        } findAndModifyCmd;

        class DataSizeCmd : public PublicGridCommand {
        public:
            DataSizeCmd() : PublicGridCommand("dataSize", "datasize") { }
            bool run(const string& dbName, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string fullns = cmdObj.firstElement().String();

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result);
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13407 ,  "how could chunk manager be null!" , cm );

                BSONObj min = cmdObj.getObjectField( "min" );
                BSONObj max = cmdObj.getObjectField( "max" );
                BSONObj keyPattern = cmdObj.getObjectField( "keyPattern" );

                uassert(13408,  "keyPattern must equal shard key", cm->getShardKey().key() == keyPattern);

                // yes these are doubles...
                double size = 0;
                double numObjects = 0;
                int millis = 0;

                set<Shard> shards;
                cm->getShardsForRange(shards, min, max);
                for ( set<Shard>::iterator i=shards.begin(), end=shards.end() ; i != end; ++i ) {
                    ScopedDbConnection conn( *i );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                    conn.done();

                    if ( ! ok ) {
                        result.appendElements( res );
                        return false;
                    }

                    size       += res["size"].number();
                    numObjects += res["numObjects"].number();
                    millis     += res["millis"].numberInt();

                }

                result.append( "size", size );
                result.append( "numObjects" , numObjects );
                result.append( "millis" , millis );
                return true;
            }

        } DataSizeCmd;

        class ConvertToCappedCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            ConvertToCappedCmd() : NotAllowedOnShardedCollectionCmd("convertToCapped") {}

            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) {
                return dbName + "." + cmdObj.firstElement().valuestrsafe();
            }

        } convertToCappedCmd;


        class GroupCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            GroupCmd() : NotAllowedOnShardedCollectionCmd("group") {}

            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) {
                return dbName + "." + cmdObj.firstElement().embeddedObjectUserCheck()["ns"].valuestrsafe();
            }

        } groupCmd;

        class DistinctCmd : public PublicGridCommand {
        public:
            DistinctCmd() : PublicGridCommand("distinct") {}
            virtual void help( stringstream &help ) const {
                help << "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
            }
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 10420 ,  "how could chunk manager be null!" , cm );

                BSONObj query = getQuery(cmdObj);
                set<Shard> shards;
                cm->getShardsForQuery(shards, query);

                set<BSONObj,BSONObjCmp> all;
                int size = 32;

                for ( set<Shard>::iterator i=shards.begin(), end=shards.end() ; i != end; ++i ) {
                    ShardConnection conn( *i , fullns );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                    conn.done();

                    if ( ! ok ) {
                        result.appendElements( res );
                        return false;
                    }

                    BSONObjIterator it( res["values"].embeddedObject() );
                    while ( it.more() ) {
                        BSONElement nxt = it.next();
                        BSONObjBuilder temp(32);
                        temp.appendAs( nxt , "" );
                        all.insert( temp.obj() );
                    }

                }

                BSONObjBuilder b( size );
                int n=0;
                for ( set<BSONObj,BSONObjCmp>::iterator i = all.begin() ; i != all.end(); i++ ) {
                    b.appendAs( i->firstElement() , b.numStr( n++ ) );
                }

                result.appendArray( "values" , b.obj() );
                return true;
            }
        } disinctCmd;

        class FileMD5Cmd : public PublicGridCommand {
        public:
            FileMD5Cmd() : PublicGridCommand("filemd5") {}
            virtual void help( stringstream &help ) const {
                help << " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
            }
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string fullns = dbName;
                fullns += ".";
                {
                    string root = cmdObj.getStringField( "root" );
                    if ( root.size() == 0 )
                        root = "fs";
                    fullns += root;
                }
                fullns += ".chunks";

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13091 , "how could chunk manager be null!" , cm );
                uassert( 13092 , "GridFS chunks collection can only be sharded on files_id", cm->getShardKey().key() == BSON("files_id" << 1));

                ChunkPtr chunk = cm->findChunk( BSON("files_id" << cmdObj.firstElement()) );

                ShardConnection conn( chunk->getShard() , fullns );
                BSONObj res;
                bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                conn.done();

                result.appendElements(res);
                return ok;
            }
        } fileMD5Cmd;

        class Geo2dFindNearCmd : public PublicGridCommand {
        public:
            Geo2dFindNearCmd() : PublicGridCommand( "geoNear" ) {}
            void help(stringstream& h) const { h << "http://www.mongodb.org/display/DOCS/Geospatial+Indexing#GeospatialIndexing-geoNearCommand"; }

            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13500 ,  "how could chunk manager be null!" , cm );

                BSONObj query = getQuery(cmdObj);
                set<Shard> shards;
                cm->getShardsForQuery(shards, query);

                int limit = 100;
                if (cmdObj["num"].isNumber())
                    limit = cmdObj["num"].numberInt();

                list< shared_ptr<Future::CommandResult> > futures;
                BSONArrayBuilder shardArray;
                for ( set<Shard>::const_iterator i=shards.begin(), end=shards.end() ; i != end ; i++ ) {
                    futures.push_back( Future::spawnCommand( i->getConnString() , dbName , cmdObj ) );
                    shardArray.append(i->getName());
                }

                multimap<double, BSONObj> results; // TODO: maybe use merge-sort instead
                string nearStr;
                double time = 0;
                double btreelocs = 0;
                double nscanned = 0;
                double objectsLoaded = 0;
                for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ) {
                    shared_ptr<Future::CommandResult> res = *i;
                    if ( ! res->join() ) {
                        errmsg = res->result()["errmsg"].String();
                        return false;
                    }

                    nearStr = res->result()["near"].String();
                    time += res->result()["stats"]["time"].Number();
                    btreelocs += res->result()["stats"]["btreelocs"].Number();
                    nscanned += res->result()["stats"]["nscanned"].Number();
                    objectsLoaded += res->result()["stats"]["objectsLoaded"].Number();

                    BSONForEach(obj, res->result()["results"].embeddedObject()) {
                        results.insert(make_pair(obj["dis"].Number(), obj.embeddedObject().getOwned()));
                    }

                    // TODO: maybe shrink results if size() > limit
                }

                result.append("ns" , fullns);
                result.append("near", nearStr);

                int outCount = 0;
                double totalDistance = 0;
                double maxDistance = 0;
                {
                    BSONArrayBuilder sub (result.subarrayStart("results"));
                    for (multimap<double, BSONObj>::const_iterator it(results.begin()), end(results.end()); it!= end && outCount < limit; ++it, ++outCount) {
                        totalDistance += it->first;
                        maxDistance = it->first; // guaranteed to be highest so far

                        sub.append(it->second);
                    }
                    sub.done();
                }

                {
                    BSONObjBuilder sub (result.subobjStart("stats"));
                    sub.append("time", time);
                    sub.append("btreelocs", btreelocs);
                    sub.append("nscanned", nscanned);
                    sub.append("objectsLoaded", objectsLoaded);
                    sub.append("avgDistance", totalDistance / outCount);
                    sub.append("maxDistance", maxDistance);
                    sub.append("shards", shardArray.arr());
                    sub.done();
                }

                return true;
            }
        } geo2dFindNearCmd;

        class MRCmd : public PublicGridCommand {
        public:
            MRCmd() : PublicGridCommand( "mapreduce" ) {}

            string getTmpName( const string& coll ) {
                static int inc = 1;
                stringstream ss;
                ss << "tmp.mrs." << coll << "_" << time(0) << "_" << inc++;
                return ss.str();
            }

            BSONObj fixForShards( const BSONObj& orig , const string& output, BSONObj& customOut , string& badShardedField ) {
                BSONObjBuilder b;
                BSONObjIterator i( orig );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    string fn = e.fieldName();
                    if ( fn == "map" ||
                            fn == "mapreduce" ||
                            fn == "mapparams" ||
                            fn == "reduce" ||
                            fn == "query" ||
                            fn == "sort" ||
                            fn == "scope" ||
                            fn == "verbose" ) {
                        b.append( e );
                    }
                    else if ( fn == "out" ||
                              fn == "finalize" ) {
                        // we don't want to copy these
                    	if (fn == "out" && e.type() == Object) {
                    		// check if there is a custom output
                    		BSONObj out = e.embeddedObject();
                    		if (out.hasField("db"))
                    			customOut = out;
                    	}
                    }
                    else {
                        badShardedField = fn;
                        return BSONObj();
                    }
                }
                b.append( "out" , output );
                return b.obj();
            }

            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                Timer t;

                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                const string shardedOutputCollection = getTmpName( collection );

                string badShardedField;
                BSONObj customOut;
                BSONObj shardedCommand = fixForShards( cmdObj , shardedOutputCollection, customOut , badShardedField );

                bool customOutDB = ! customOut.isEmpty() && customOut.hasField( "db" );

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    if ( customOutDB ) {
                        errmsg = "can't use out 'db' with non-sharded db";
                        return false;
                    }
                    return passthrough( conf , cmdObj , result );
                }

                if ( badShardedField.size() ) {
                    errmsg = str::stream() << "unknown m/r field for sharding: " << badShardedField;
                    return false;
                }

                BSONObjBuilder timingBuilder;

                ChunkManagerPtr cm = conf->getChunkManager( fullns );

                BSONObj q;
                if ( cmdObj["query"].type() == Object ) {
                    q = cmdObj["query"].embeddedObjectUserCheck();
                }

                set<Shard> shards;
                cm->getShardsForQuery( shards , q );


                BSONObjBuilder finalCmd;
                finalCmd.append( "mapreduce.shardedfinish" , cmdObj );
                finalCmd.append( "shardedOutputCollection" , shardedOutputCollection );
                
                
                {
                    // we need to use our connections to the shard
                    // so filtering is done correctly for un-owned docs
                    // so we allocate them in our thread
                    // and hand off

                    vector< shared_ptr<ShardConnection> > shardConns;

                    list< shared_ptr<Future::CommandResult> > futures;
                    
                    for ( set<Shard>::iterator i=shards.begin(), end=shards.end() ; i != end ; i++ ) {
                        shared_ptr<ShardConnection> temp( new ShardConnection( i->getConnString() , fullns ) );
                        assert( temp->get() );
                        futures.push_back( Future::spawnCommand( i->getConnString() , dbName , shardedCommand , temp->get() ) );
                        shardConns.push_back( temp );
                    }
                    
                    bool failed = false;
                    
                    BSONObjBuilder shardresults;
                    for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ) {
                        shared_ptr<Future::CommandResult> res = *i;
                        if ( ! res->join() ) {
                            error() << "sharded m/r failed on shard: " << res->getServer() << " error: " << res->result() << endl;
                            result.append( "cause" , res->result() );
                            errmsg = "mongod mr failed: ";
                            errmsg += res->result().toString();
                            failed = true;
                            continue;
                        }
                        shardresults.append( res->getServer() , res->result() );
                    }

                    for ( unsigned i=0; i<shardConns.size(); i++ )
                        shardConns[i]->done();

                    if ( failed )
                        return 0;

                    finalCmd.append( "shards" , shardresults.obj() );
                    timingBuilder.append( "shards" , t.millis() );
                }

                Timer t2;
                // by default the target database is same as input
                Shard outServer = conf->getPrimary();
                string outns = fullns;
                if ( customOutDB ) {
                	// have to figure out shard for the output DB
                	BSONElement elmt = customOut.getField("db");
                	string outdb = elmt.valuestrsafe();
                	outns = outdb + "." + collection;
                    DBConfigPtr conf2 = grid.getDBConfig( outdb , true );
                	outServer = conf2->getPrimary();
                }
                log() << "customOut: " << customOut << " outServer: " << outServer << endl;
                        
                ShardConnection conn( outServer , outns );
                BSONObj finalResult;
                bool ok = conn->runCommand( dbName , finalCmd.obj() , finalResult );
                conn.done();

                if ( ! ok ) {
                    errmsg = "final reduce failed: ";
                    errmsg += finalResult.toString();
                    return 0;
                }
                timingBuilder.append( "final" , t2.millis() );

                result.appendElements( finalResult );
                result.append( "timeMillis" , t.millis() );
                result.append( "timing" , timingBuilder.obj() );

                return 1;
            }
        } mrCmd;

        class ApplyOpsCmd : public PublicGridCommand {
        public:
            ApplyOpsCmd() : PublicGridCommand( "applyOps" ) {}

            virtual bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg = "applyOps not allowed through mongos";
                return false;
            }

        } applyOpsCmd;

    }

}