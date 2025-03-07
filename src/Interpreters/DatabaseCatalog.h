/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#pragma once

#include <Core/UUID.h>
#include <Interpreters/Context_fwd.h>
#include <Interpreters/StorageID.h>
#include <Parsers/IAST_fwd.h>
#include <Storages/IStorage_fwd.h>

#include <boost/noncopyable.hpp>
#include <Poco/Logger.h>

#include <array>
#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>


namespace DB
{

class IDatabase;
class Exception;
class ColumnsDescription;
struct ConstraintsDescription;
struct ForeignKeysDescription;
struct UniqueNotEnforcedDescription;

using DatabasePtr = std::shared_ptr<IDatabase>;
using DatabaseAndTable = std::pair<DatabasePtr, StoragePtr>;
using Databases = std::map<String, std::shared_ptr<IDatabase>>;

/// Table -> set of table-views that make SELECT from it.
using ViewDependencies = std::map<StorageID, std::set<StorageID>>;
using Dependencies = std::vector<StorageID>;

using MemoryTableDependencies = std::map<StorageID, std::set<StorageID>>;
using MemoryTableInfo = std::pair<StoragePtr, bool>;

/// Allows executing DDL query only in one thread.
/// Puts an element into the map, locks tables's mutex, counts how much threads run parallel query on the table,
/// when counter is 0 erases element in the destructor.
/// If the element already exists in the map, waits when ddl query will be finished in other thread.
class DDLGuard
{
public:
    struct Entry
    {
        std::unique_ptr<std::mutex> mutex;
        UInt32 counter;
    };

    /// Element name -> (mutex, counter).
    /// NOTE: using std::map here (and not std::unordered_map) to avoid iterator invalidation on insertion.
    using Map = std::map<String, Entry>;

    DDLGuard(
        Map & map_,
        std::shared_mutex & db_mutex_,
        std::unique_lock<std::mutex> guards_lock_,
        const String & elem,
        const String & database_name);
    ~DDLGuard();

    /// Unlocks table name, keeps holding read lock for database name
    void releaseTableLock() noexcept;

private:
    Map & map;
    std::shared_mutex & db_mutex;
    Map::iterator it;
    std::unique_lock<std::mutex> guards_lock;
    std::unique_lock<std::mutex> table_lock;
    bool table_lock_removed = false;
    bool is_database_guard = false;
};

using DDLGuardPtr = std::unique_ptr<DDLGuard>;


/// Creates temporary table in `_temporary_and_external_tables` with randomly generated unique StorageID.
/// Such table can be accessed from everywhere by its ID.
/// Removes the table from database on destruction.
/// TemporaryTableHolder object can be attached to a query or session Context, so table will be accessible through the context.
struct TemporaryTableHolder : boost::noncopyable, WithContext
{
    using Creator = std::function<StoragePtr (const StorageID &)>;

    TemporaryTableHolder(ContextPtr context, const Creator & creator, const ASTPtr & query = {});

    /// Creates temporary table with Engine=Memory
    TemporaryTableHolder(
        ContextPtr context,
        const ColumnsDescription & columns,
        const ConstraintsDescription & constraints,
        const ForeignKeysDescription & foreign_keys_,
        const UniqueNotEnforcedDescription & unique_not_enforced_,
        const ASTPtr & query = {},
        bool create_for_global_subquery = false);

    TemporaryTableHolder(TemporaryTableHolder && rhs);
    TemporaryTableHolder & operator = (TemporaryTableHolder && rhs);

    ~TemporaryTableHolder();

    StorageID getGlobalTableID() const;

    StoragePtr getTable() const;

    operator bool () const { return id != UUIDHelpers::Nil; }

    IDatabase * temporary_tables = nullptr;
    UUID id = UUIDHelpers::Nil;
};

///TODO maybe remove shared_ptr from here?
using TemporaryTablesMapping = std::map<String, std::shared_ptr<TemporaryTableHolder>>;

class BackgroundSchedulePoolTaskHolder;

/// For some reason Context is required to get Storage from Database object
class DatabaseCatalog : boost::noncopyable, WithMutableContext
{
public:
    static constexpr const char * TEMPORARY_DATABASE = "_temporary_and_external_tables";
    static constexpr const char * SYSTEM_DATABASE = "system";
    static constexpr const char * INFORMATION_SCHEMA = "information_schema";
    static constexpr const char * INFORMATION_SCHEMA_UPPERCASE = "INFORMATION_SCHEMA";

    static DatabaseCatalog & init(ContextMutablePtr global_context_);
    static DatabaseCatalog & instance();
    static void shutdown();

    void initializeAndLoadTemporaryDatabase();
    void loadDatabases();

    /// Get an object that protects the table from concurrently executing multiple DDL operations.
    DDLGuardPtr getDDLGuard(const String & database, const String & table);
    /// Get an object that protects the database from concurrent DDL queries all tables in the database
    std::unique_lock<std::shared_mutex> getExclusiveDDLGuardForDatabase(const String & database);


    void assertDatabaseExists(const String & database_name, ContextPtr local_context) const;
    void assertDatabaseDoesntExist(const String & database_name, ContextPtr local_context) const;

    DatabasePtr getDatabaseForTemporaryTables() const;
    DatabasePtr getSystemDatabase() const;

    void attachDatabase(const String & database_name, const DatabasePtr & database);
    DatabasePtr detachDatabase(ContextPtr local_context, const String & database_name, bool drop = false, bool check_empty = true);
    void updateDatabaseName(const String & old_name, const String & new_name);

    /// database_name must be not empty
    DatabasePtr getDatabase(const String & database_name, ContextPtr local_context) const;
    DatabasePtr tryGetDatabase(const String & database_name, ContextPtr local_context) const;
    bool isDatabaseExist(const String & database_name, ContextPtr local_context) const;
    Databases getDatabases(ContextPtr local_context) const;
    Databases getNonCnchDatabases() const;

    /// Same as getDatabase(const String & database_name), but if database_name is empty, current database of local_context is used

    /// For all of the following methods database_name in table_id must be not empty (even for temporary tables).
    void assertTableDoesntExist(const StorageID & table_id, ContextPtr context) const;
    bool isTableExist(const StorageID & table_id, ContextPtr context) const;
    bool isDictionaryExist(const StorageID & table_id) const;

    StoragePtr getTable(const StorageID & table_id, ContextPtr context) const;
    StoragePtr tryGetTable(const StorageID & table_id, ContextPtr context) const;
    DatabaseAndTable getDatabaseAndTable(const StorageID & table_id, ContextPtr context) const;
    DatabaseAndTable tryGetDatabaseAndTable(const StorageID & table_id, ContextPtr context) const;
    DatabaseAndTable getTableImpl(const StorageID & table_id,
                                  ContextPtr context,
                                  std::optional<Exception> * exception = nullptr) const;

    void addDependency(const StorageID & from, const StorageID & where);
    void removeDependency(const StorageID & from, const StorageID & where);
    Dependencies getDependencies(const StorageID & from) const;

    /// For Materialized and Live View
    void updateDependency(const StorageID & old_from, const StorageID & old_where,const StorageID & new_from, const StorageID & new_where);

    /// If table has UUID, addUUIDMapping(...) must be called when table attached to some database
    /// removeUUIDMapping(...) must be called when it detached,
    /// and removeUUIDMappingFinally(...) must be called when table is dropped and its data removed from disk.
    /// Such tables can be accessed by persistent UUID instead of database and table name.
    void addUUIDMapping(const UUID & uuid, const DatabasePtr & database, const StoragePtr & table);
    void removeUUIDMapping(const UUID & uuid);
    void removeUUIDMappingFinally(const UUID & uuid);
    /// For moving table between databases
    void updateUUIDMapping(const UUID & uuid, DatabasePtr database, StoragePtr table);
    /// This method adds empty mapping (with database and storage equal to nullptr).
    /// It's required to "lock" some UUIDs and protect us from collision.
    /// Collisions of random 122-bit integers are very unlikely to happen,
    /// but we allow to explicitly specify UUID in CREATE query (in particular for testing).
    /// If some UUID was already added and we are trying to add it again,
    /// this method will throw an exception.
    void addUUIDMapping(const UUID & uuid);

    static String getPathForUUID(const UUID & uuid);

    DatabaseAndTable tryGetByUUID(const UUID & uuid, const ContextPtr & local_context) const;

    String getPathForDroppedMetadata(const StorageID & table_id) const;
    void enqueueDroppedTableCleanup(StorageID table_id, StoragePtr table, String dropped_metadata_path, bool ignore_delay = false);

    void waitTableFinallyDropped(const UUID & uuid);

private:
    // The global instance of database catalog. unique_ptr is to allow
    // deferred initialization. Thought I'd use std::optional, but I can't
    // make emplace(global_context_) compile with private constructor ¯\_(ツ)_/¯.
    static std::unique_ptr<DatabaseCatalog> database_catalog;

    explicit DatabaseCatalog(ContextMutablePtr global_context_);
    void assertDatabaseExistsUnlocked(const String & database_name) const;
    void assertDatabaseDoesntExistUnlocked(const String & database_name) const;
    DatabasePtr getDatabase(const String & database_name) const;
    DatabasePtr tryGetDatabase(const String & database_name) const;
    DatabasePtr getDatabase(const UUID & uuid) const;
    DatabasePtr tryGetDatabase(const UUID & uuid) const;

    DatabasePtr tryGetDatabaseCnch(const String & database_name) const;
    DatabasePtr tryGetDatabaseCnch(const String & database_name, ContextPtr context) const;
    DatabasePtr tryGetDatabaseCnch(const UUID & uuid) const;
    Databases getDatabaseCnchs() const;

    void shutdownImpl();


    struct UUIDToStorageMapPart
    {
        std::unordered_map<UUID, DatabaseAndTable> map;
        mutable std::mutex mutex;
    };

    static constexpr UInt64 bits_for_first_level = 4;
    using UUIDToStorageMap = std::array<UUIDToStorageMapPart, 1ull << bits_for_first_level>;

    static inline size_t getFirstLevelIdx(const UUID & uuid)
    {
        return uuid.toUnderType().items[0] >> (64 - bits_for_first_level);
    }

    inline bool preferCnchCatalog(const Context & context) const;

    struct TableMarkedAsDropped
    {
        StorageID table_id = StorageID::createEmpty();
        StoragePtr table;
        String metadata_path;
        time_t drop_time{};
    };
    using TablesMarkedAsDropped = std::list<TableMarkedAsDropped>;

    void loadMarkedAsDroppedTables();
    void dropTableDataTask();
    void dropTableFinally(const TableMarkedAsDropped & table);

    static constexpr size_t reschedule_time_ms = 100;
    static constexpr time_t drop_error_cooldown_sec = 5;

    using UUIDToDatabaseMap = std::unordered_map<UUID, DatabasePtr>;

    mutable std::mutex databases_mutex;

    ViewDependencies view_dependencies;

    Databases databases;
    UUIDToDatabaseMap db_uuid_map;
    UUIDToStorageMap uuid_map;

    Poco::Logger * log;

    /// Do not allow simultaneous execution of DDL requests on the same table.
    /// database name -> database guard -> (table name mutex, counter),
    /// counter: how many threads are running a query on the table at the same time
    /// For the duration of the operation, an element is placed here, and an object is returned,
    /// which deletes the element in the destructor when counter becomes zero.
    /// In case the element already exists, waits when query will be executed in other thread. See class DDLGuard below.
    using DatabaseGuard = std::pair<DDLGuard::Map, std::shared_mutex>;
    using DDLGuards = std::map<String, DatabaseGuard>;
    DDLGuards ddl_guards;
    /// If you capture mutex and ddl_guards_mutex, then you need to grab them strictly in this order.
    mutable std::mutex ddl_guards_mutex;

    TablesMarkedAsDropped tables_marked_dropped;
    std::unordered_set<UUID> tables_marked_dropped_ids;
    mutable std::mutex tables_marked_dropped_mutex;

    std::unique_ptr<BackgroundSchedulePoolTaskHolder> drop_task;
    static constexpr time_t default_drop_delay_sec = 8 * 60;
    time_t drop_delay_sec = default_drop_delay_sec;
    std::condition_variable wait_table_finally_dropped;
    const bool use_cnch_catalog;
};

}
