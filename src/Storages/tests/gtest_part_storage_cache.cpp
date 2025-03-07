#include <chrono>
#include <thread>
#include <CloudServices/CnchCreateQueryHelper.h>
#include <Protos/DataModelHelpers.h>
#include <Storages/CnchStorageCache.h>
#include <Storages/PartCacheManager.h>
#include <Storages/StorageCnchMergeTree.h>
#include <gtest/gtest.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>

using namespace std::chrono;
using namespace DB;
using namespace testing;

namespace CacheTestMock
{

StoragePtr createTable(const String & create_query, ContextMutablePtr context)
{
    return createStorageFromQuery(create_query, context);
}

DataModelPartPtr
createPart(const String & partition_id, UInt64 min_block, UInt64 max_block, UInt64 level, UInt64 mutation = 0, UInt64 hint_mutation = 0)
{
    DataModelPartPtr part_model = std::make_shared<Protos::DataModelPart>();
    Protos::DataModelPartInfo * info_model = part_model->mutable_part_info();

    info_model->set_partition_id(partition_id);
    info_model->set_min_block(min_block);
    info_model->set_max_block(max_block);
    info_model->set_level(level);
    info_model->set_mutation(mutation);
    info_model->set_hint_mutation(hint_mutation);

    part_model->set_rows_count(0);
    part_model->set_partition_minmax("xxxx");
    part_model->set_marks_count(0);

    return part_model;
}

Protos::DataModelPartVector createPartBatch(const String & partition_id, size_t count, size_t start_block_index = 0)
{
    Protos::DataModelPartVector res;
    for (size_t i = start_block_index; i < start_block_index + count; i++)
    {
        DataModelPartPtr part_model = createPart(partition_id, i, i, 0);
        *(res.mutable_parts()->Add()) = *part_model;
    }
    return res;
}

DataModelPartWithNameVector createPartsBatch(const String & partition_id, size_t count) {
    DataModelPartWithNameVector ret;
    for (size_t i = 0; i< count; i++) {
        DataModelPartPtr part_model = createPart(partition_id, i, i, 0);
        auto part_info = createPartInfoFromModel(part_model->part_info());
        String part_name = part_info->getPartName();
        ret.emplace_back(std::make_shared<DataModelPartWithName>(std::move(part_name), std::move(part_model)));
    }
    return ret;
}
}

class CacheManagerTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tryRegisterStorages();
        tryRegisterDisks();
        getContext().resetStoragePolicy();
    }
};

/***
 * @brief Test mock create storage
*/
TEST_F(CacheManagerTest, CreateTable)
{
    String query = "create table gztest.test(id Int32) ENGINE=CnchMergeTree order by id";
    StoragePtr storage = CacheTestMock::createTable(query, getContext().context);

    EXPECT_EQ(storage->getDatabaseName(), "gztest");
    EXPECT_EQ(storage->getTableName(), "test");
    EXPECT_EQ(storage->getStorageUUID(), UUIDHelpers::Nil);
}

/**
 * @brief Storage cache test 1
 * 1. Test create new storage and insert into storage cache.
 * 2. Test get storage from storage cache
 * 3. Test get storage from storage cache when topology change
 */
TEST_F(CacheManagerTest, GetTableFromCache)
{
    std::shared_ptr<PartCacheManager> cache_manager = std::make_shared<PartCacheManager>(getContext().context, true);
    String query = "create table gztest.test UUID '61f0c404-5cb3-11e7-907b-a6006ad3dba0' (id Int32) ENGINE=CnchMergeTree order by id";
    StoragePtr storage = CacheTestMock::createTable(query, getContext().context);

    UInt64 commit_ts = 1;
    auto current_topology_version = PairInt64{1, 1};

    // load active tables into CacheManager
    cache_manager->mayUpdateTableMeta(*storage, current_topology_version);
    auto entry = cache_manager->getTableMeta(storage->getStorageUUID());

    // get storage and insert storage into cache
    cache_manager->insertStorageCache(storage->getStorageID(), storage, commit_ts, current_topology_version);

    // get storage from cache
    auto storage_from_cache = cache_manager->getStorageFromCache(storage->getStorageUUID(), current_topology_version);

    EXPECT_NE(storage_from_cache, nullptr);
    EXPECT_EQ(storage->getStorageID(), storage_from_cache->getStorageID());

    // mock topology change
    auto new_topology_version = PairInt64{1, 2};
    auto storage_from_invilid_cache = cache_manager->getStorageFromCache(storage->getStorageUUID(), new_topology_version);

    EXPECT_EQ(storage_from_invilid_cache, nullptr);

    EXPECT_NE(cache_manager->getStorageFromCache(storage->getStorageUUID(), current_topology_version), nullptr);
    EXPECT_EQ(cache_manager->getStorageFromCache(storage->getStorageUUID(), current_topology_version)->getStorageUUID(), storage->getStorageUUID());

    cache_manager->shutDown();
}

/***
 * @brief Storage cache test 2
 * Test contensions between get storage from cache and alter table.
 * The storage with lower commit_ts should never overwrite the storage with higher commit_ts in storage cache
*/
TEST_F(CacheManagerTest, AlterTableContention)
{
    auto context = getContext().context;
    std::shared_ptr<PartCacheManager> cache_manager = std::make_shared<PartCacheManager>(context, true);
    auto topology_version = PairInt64{1, 1};
    UInt64 ts_commit = 1, ts_t2 = 2;

    String query = "create table gztest.test UUID '61f0c404-5cb3-11e7-907b-a6006ad3dba0' (id Int32) ENGINE=CnchMergeTree order by id";
    StoragePtr storage = CacheTestMock::createTable(query, context);

    //T0: create table
    cache_manager->mayUpdateTableMeta(*storage, topology_version);
    auto entry = cache_manager->getTableMeta(storage->getStorageUUID());

    //T1: try get table from metastore
    const StoragePtr& storage_from_metastore = storage;

    //T2: alter table and insert into cache
    String alter_query_t2 = "create table gztest.test UUID '61f0c404-5cb3-11e7-907b-a6006ad3dba0' (id Int32, extra_int Int32) "
                            "ENGINE=CnchMergeTree order by id";
    StoragePtr storage_t2 = CacheTestMock::createTable(alter_query_t2, context);
    cache_manager->insertStorageCache(storage_t2->getStorageID(), storage_t2, ts_t2, topology_version);

    //T3: try insert old storage which get from metastore at T1 into cache
    cache_manager->insertStorageCache(storage_from_metastore->getStorageID(), storage_from_metastore, ts_commit, topology_version);

    //T4: get storate from cache. It should be new altered storage
    auto storage_from_cache = cache_manager->getStorageFromCache(storage->getStorageUUID(), topology_version);

    EXPECT_NE(storage_from_cache, nullptr);
    IStorage::ColumnSizeByName column_sizes = storage_from_cache->getColumnSizes();
    auto metadata_ptr = storage_from_cache->getInMemoryMetadataPtr();
    EXPECT_NE(metadata_ptr, nullptr);

    EXPECT_EQ(metadata_ptr->getColumns().has("extra_int"), true);

    cache_manager->shutDown();
}


/***
 * @brief part cache test
 * 1. Test insert into part cache
 * 2. Test get parts from part cache
 * 3. Test get parts from cache by partition
 * 4. Test get parts from outdated cache
*/
TEST_F(CacheManagerTest, GetPartsFromCache)
{
    auto context = getContext().context;
    std::shared_ptr<PartCacheManager> cache_manager = std::make_shared<PartCacheManager>(context, true);
    auto topology_version = PairInt64{1, 1};

    // mock storage
    String query = "create table gztest.test (id Int32) ENGINE=CnchMergeTree partition by id order by tuple()";
    StoragePtr storage = CacheTestMock::createTable(query, context);
    String storage_uuid = UUIDHelpers::UUIDToString(storage->getStorageUUID());

    // add table entry in cache manager and mock load partitions
    cache_manager->mayUpdateTableMeta(*storage, topology_version);
    auto entry = cache_manager->getTableMeta(storage->getStorageUUID());

    auto ts1 = cache_manager->getTableLastUpdateTime(storage->getStorageUUID());
    EXPECT_NE(ts1, 0);

    auto it_p0 = entry->partitions.emplace("1000", std::make_shared<CnchPartitionInfo>(storage_uuid, nullptr, "1000")).first;
    auto it_p1 = entry->partitions.emplace("1001", std::make_shared<CnchPartitionInfo>(storage_uuid, nullptr, "1001")).first;
    auto it_p2 = entry->partitions.emplace("1002", std::make_shared<CnchPartitionInfo>(storage_uuid, nullptr, "1002")).first;

    // mock insert part into cache
    Protos::DataModelPartVector parts_models = CacheTestMock::createPartBatch("1001", 10, 0);
    EXPECT_EQ(parts_models.parts().size(), 10);
    (*it_p1)->cache_status = CacheStatus::LOADING;
    cache_manager->insertDataPartsIntoCache(*storage, parts_models.parts(), false, false, topology_version);
    (*it_p1)->cache_status = CacheStatus::LOADED;

    // `getTableLastUpdateTime` should return a non-zero value after insert a valid part.
    EXPECT_GT(cache_manager->getTableLastUpdateTime(storage->getStorageUUID()), ts1);

    parts_models = CacheTestMock::createPartBatch("1002", 20, 0);
    EXPECT_EQ(parts_models.parts().size(), 20);
    (*it_p2)->cache_status = CacheStatus::LOADING;
    cache_manager->insertDataPartsIntoCache(*storage, parts_models.parts(), false, false, topology_version);
    (*it_p2)->cache_status = CacheStatus::LOADED;

    bool load_from_func = false;
    auto load_func = [&](const Strings &, const Strings &) -> DataModelPartWithNameVector {
        load_from_func = true;
        return {};
    };

    // Test `getPartitionList`
    std::vector<std::shared_ptr<MergeTreePartition>> partition_list;
    EXPECT_EQ(cache_manager->getPartitionList(*storage, partition_list, topology_version), true);
    EXPECT_EQ(partition_list.size(), 3);
    std::vector<String> partition_ids;
    EXPECT_EQ(cache_manager->getPartitionIDs(*storage, partition_ids, topology_version), true);
    EXPECT_EQ(partition_ids.size(), 3);

    // test get parts from cache
    ServerDataPartsVector parts_from_cache = cache_manager->getOrSetServerDataPartsInPartitions(
        *storage, {"1001", "1002"}, load_func, TxnTimestamp::maxTS(), topology_version);
    EXPECT_EQ(load_from_func, false);
    EXPECT_EQ(parts_from_cache.size(), 30);

    parts_from_cache
        = cache_manager->getOrSetServerDataPartsInPartitions(*storage, {"1001"}, load_func, TxnTimestamp::maxTS(), topology_version);
    EXPECT_EQ(load_from_func, false);
    EXPECT_EQ(parts_from_cache.size(), 10);

    // test get parts from cache by partitions
    entry->load_parts_by_partition = true;
    ServerDataPartsVector parts_from_cache_by_partition = cache_manager->getOrSetServerDataPartsInPartitions(
        *storage, {"1001", "1002"}, load_func, TxnTimestamp::maxTS(), topology_version);
    EXPECT_EQ(load_from_func, false);
    EXPECT_EQ(parts_from_cache_by_partition.size(), 30);

    parts_from_cache_by_partition
        = cache_manager->getOrSetServerDataPartsInPartitions(*storage, {"1001"}, load_func, TxnTimestamp::maxTS(), topology_version);
    EXPECT_EQ(parts_from_cache_by_partition.size(), 10);

    // mock topology change and get part from cache again
    EXPECT_EQ(load_from_func, false);
    auto new_topology_version = PairInt64{1, 2};
    ServerDataPartsVector parts_from_invalid_cache = cache_manager->getOrSetServerDataPartsInPartitions(
        *storage, {"1000", "1001", "1002"}, load_func, TxnTimestamp::maxTS(), new_topology_version);

    EXPECT_EQ(parts_from_invalid_cache.size(), 0);
    EXPECT_EQ(load_from_func, true);
    /// TODO: Partitions in new topology will be clear (not merged yet).
    EXPECT_EQ(cache_manager->getPartitionList(*storage, partition_list, topology_version), true);
    EXPECT_EQ(partition_list.size(), 3);
    EXPECT_EQ(cache_manager->getPartitionIDs(*storage, partition_ids, topology_version), true);
    EXPECT_EQ(partition_ids.size(), 3);

    load_from_func = false;

    cache_manager->shutDown();
}

/// Test the functionality of `updateTableNameInMetaEntry`.
TEST_F(CacheManagerTest, RenameTable)
{
    std::shared_ptr<PartCacheManager> cache_manager = std::make_shared<PartCacheManager>(getContext().context, true);
    String query = "create table gztest.test UUID '61f0c404-5cb3-11e7-907b-a6006ad3dba0' (id Int32) ENGINE=CnchMergeTree order by id";
    StoragePtr storage = CacheTestMock::createTable(query, getContext().context);

    auto current_topology_version = PairInt64{1, 1};
    // load active tables into CacheManager
    cache_manager->mayUpdateTableMeta(*storage, current_topology_version);
    TableMetaEntryPtr entry = cache_manager->getTableMeta(storage->getStorageUUID());

    EXPECT_EQ(entry->table, "test");
    EXPECT_EQ(entry->database, "gztest");

    cache_manager->updateTableNameInMetaEntry(UUIDHelpers::UUIDToString(storage->getStorageUUID()), "gztest2", "test2");
    entry = cache_manager->getTableMeta(storage->getStorageUUID());

    EXPECT_EQ(entry->table, "test2");
    EXPECT_EQ(entry->database, "gztest2");

    cache_manager->shutDown();
}

TEST_F(CacheManagerTest, GetAllActiveTables) {

    std::shared_ptr<PartCacheManager> cache_manager = std::make_shared<PartCacheManager>(getContext().context, true);
    String tb1_query = "create table gztest.test UUID '61f0c404-5cb3-11e7-907b-a6006ad3dba0' (id Int32) ENGINE=CnchMergeTree order by id";
    String tb2_query = "create table gztest.test2 UUID 'babaf814-7a2c-11ee-b6e0-bfbb87d31a0b' (id Int32) ENGINE=CnchMergeTree order by id";
    StoragePtr storage_1 = CacheTestMock::createTable(tb1_query, getContext().context);
    StoragePtr storage_2 = CacheTestMock::createTable(tb2_query, getContext().context);

    auto current_topology_version = PairInt64{1, 1};
    auto next_topology_version = PairInt64{1, 2};
    std::vector<TableMetaEntryPtr> tables = cache_manager->getAllActiveTables();
    EXPECT_EQ(tables.size(), 0);

    cache_manager->mayUpdateTableMeta(*storage_1, current_topology_version);
    tables = cache_manager->getAllActiveTables();
    EXPECT_EQ(tables.size(), 1);

    cache_manager->mayUpdateTableMeta(*storage_2, current_topology_version);
    tables = cache_manager->getAllActiveTables();
    EXPECT_EQ(tables.size(), 2);

    cache_manager->mayUpdateTableMeta(*storage_1, current_topology_version);
    tables = cache_manager->getAllActiveTables();
    EXPECT_EQ(tables.size(), 2);

    /// Expected to clear the old cache. But it's not implmented currently.
    // cache_manager->mayUpdateTableMeta(*storage_1, next_topology_version);
    // tables = cache_manager->getAllActiveTables();
    // EXPECT_EQ(tables.size(), 1);

    cache_manager->shutDown();
}

TEST_F(CacheManagerTest, getAndSetStatus) {

    std::shared_ptr<PartCacheManager> cache_manager = std::make_shared<PartCacheManager>(getContext().context, true);
    String tb1_query = "create table gztest.test UUID '61f0c404-5cb3-11e7-907b-a6006ad3dba0' (id Int32) ENGINE=CnchMergeTree order by id";
    StoragePtr storage_1 = CacheTestMock::createTable(tb1_query, getContext().context);
    String storage_1_uuid = UUIDHelpers::UUIDToString(storage_1->getStorageUUID());
    auto current_topology_version = PairInt64{1, 1};
    cache_manager->mayUpdateTableMeta(*storage_1, current_topology_version);

    auto topology_version = PairInt64{1, 1};
    TableMetaEntryPtr entry = cache_manager->getTableMeta(storage_1->getStorageUUID());
    auto it_p0 = entry->partitions.emplace("1000", std::make_shared<CnchPartitionInfo>(storage_1_uuid, nullptr, "1000")).first;
    Protos::DataModelPartVector parts_models = CacheTestMock::createPartBatch("1000", 10, 0);
    (*it_p0)->cache_status = CacheStatus::LOADING;
    cache_manager->insertDataPartsIntoCache(*storage_1, parts_models.parts(), false, false, topology_version);
    (*it_p0)->cache_status = CacheStatus::LOADED;

    bool load_from_func = false;
    auto load_func = [&](const Strings &, const Strings &) -> DataModelPartWithNameVector {
        load_from_func = true;
        return {};
    };
    ServerDataPartsVector parts_from_cache = cache_manager->getOrSetServerDataPartsInPartitions(
        *storage_1, {"1000"}, load_func, TxnTimestamp::maxTS(), topology_version);
    EXPECT_EQ(load_from_func, false);
    EXPECT_EQ(parts_from_cache.size(), 10);
    EXPECT_EQ(
        cache_manager->getOrSetServerDataPartsInPartitions(*storage_1, {"1001"}, load_func, TxnTimestamp::maxTS(), topology_version).size(),
        0);
    EXPECT_EQ(load_from_func, false);

    /// Initial value.
    EXPECT_EQ(cache_manager->getTableClusterStatus(storage_1->getStorageUUID()), true);

    // Catalog is not initialized, but the value would still set.
    EXPECT_THROW({ cache_manager->setTableClusterStatus(storage_1->getStorageUUID(), false); }, Exception);

    EXPECT_EQ(cache_manager->getTableClusterStatus(storage_1->getStorageUUID()), false);

    /// Initial value.
    EXPECT_EQ(cache_manager->getTablePreallocateVW(storage_1->getStorageUUID()), "");

    cache_manager->setTablePreallocateVW(storage_1->getStorageUUID(), "vw1");

    EXPECT_EQ(cache_manager->getTablePreallocateVW(storage_1->getStorageUUID()), "vw1");

    /// Cache should not be affected without topology change.
    parts_from_cache = cache_manager->getOrSetServerDataPartsInPartitions(
        *storage_1, {"1000"}, load_func, TxnTimestamp::maxTS(), topology_version);
    EXPECT_EQ(load_from_func, false);
    EXPECT_EQ(parts_from_cache.size(), 10);
    EXPECT_EQ(
        cache_manager->getOrSetServerDataPartsInPartitions(*storage_1, {"1001"}, load_func, TxnTimestamp::maxTS(), topology_version).size(),
        0);
    EXPECT_EQ(load_from_func, false);
}

TEST_F(CacheManagerTest, InvalidPartCache) {
    std::shared_ptr<PartCacheManager> cache_manager = std::make_shared<PartCacheManager>(getContext().context, true);
    String query = "create table gztest.test UUID '61f0c404-5cb3-11e7-907b-a6006ad3dba0' (id Int32) ENGINE=CnchMergeTree order by id";
    StoragePtr storage = CacheTestMock::createTable(query, getContext().context);
    auto current_topology_version = PairInt64{1, 1};
    cache_manager->mayUpdateTableMeta(*storage, current_topology_version);
    TableMetaEntryPtr entry = cache_manager->getTableMeta(storage->getStorageUUID());
    auto it_p0 = entry->partitions.emplace("1000", std::make_shared<CnchPartitionInfo>(UUIDHelpers::UUIDToString(storage->getStorageUUID()), nullptr, "1000")).first;

    Protos::DataModelPartVector parts_models = CacheTestMock::createPartBatch("1000", 10, 0);
    EXPECT_EQ(parts_models.parts().size(), 10);
    (*it_p0)->cache_status = CacheStatus::LOADING;
    cache_manager->insertDataPartsIntoCache(*storage, parts_models.parts(), false, false, current_topology_version);
    (*it_p0)->cache_status = CacheStatus::LOADED;

    bool load_from_func = false;
    auto load_func = [&](const Strings &, const Strings &) -> DataModelPartWithNameVector {
        load_from_func = true;
        return {};
    };


    EXPECT_EQ(cache_manager->getOrSetServerDataPartsInPartitions(
        *storage, {"1000", "1001"}, load_func, TxnTimestamp::maxTS(), current_topology_version).size(), 10);
    EXPECT_EQ(load_from_func, false);

    cache_manager->invalidPartCache(storage->getStorageUUID());

    EXPECT_EQ(cache_manager->getOrSetServerDataPartsInPartitions(
        *storage, {"1000", "1001"}, load_func, TxnTimestamp::maxTS(), current_topology_version).size(), 0);
    EXPECT_EQ(load_from_func, false);

    /// Reset state.
    current_topology_version = PairInt64{2, 1};
    cache_manager->mayUpdateTableMeta(*storage, current_topology_version);

    auto new_load_func = [&](const Strings &, const Strings &) -> DataModelPartWithNameVector {
        load_from_func = true;
        return CacheTestMock::createPartsBatch("1000", 10);
    };
    EXPECT_EQ(parts_models.parts().size(), 10);

    (*it_p0)->cache_status = CacheStatus::LOADING;
    /// Cannot insert into the cache because the parts are from the old topology.
    cache_manager->insertDataPartsIntoCache(*storage, parts_models.parts(), false, false, PairInt64{1, 1});
    /// Cannot insert into the cache because cache is not loaded.
    cache_manager->insertDataPartsIntoCache(*storage, parts_models.parts(), false, false, current_topology_version);
    (*it_p0)->cache_status = CacheStatus::LOADED;
    load_from_func = false;

    EXPECT_EQ(cache_manager->getOrSetServerDataPartsInPartitions(
        *storage, {"1000", "1001"}, new_load_func, TxnTimestamp::maxTS(), current_topology_version).size(), 10);
    EXPECT_EQ(load_from_func, true);
    load_from_func = false;


    ServerDataPartsVector cached_parts = cache_manager->getOrSetServerDataPartsInPartitions(
        *storage, {"1000", "1001"}, new_load_func, TxnTimestamp::maxTS(), current_topology_version);
    cache_manager->invalidPartCache(storage->getStorageUUID(), cached_parts);

    EXPECT_EQ(cache_manager->getOrSetServerDataPartsInPartitions(
        *storage, {"1000", "1001"}, new_load_func, TxnTimestamp::maxTS(), current_topology_version).size(), 0);
    EXPECT_EQ(load_from_func, false);
}

TEST_F(CacheManagerTest, DelayTest) {

    std::vector<std::thread> tds;
    auto context = getContext().context;
    std::shared_ptr<PartCacheManager> cache_manager = std::make_shared<PartCacheManager>(context, true);

    // mock storage
    String query = "create table gztest.test UUID '61f0c404-5cb3-11e7-907b-a6006ad3dba0' (id Int32) ENGINE=CnchMergeTree order by id";
    StoragePtr storage = CacheTestMock::createTable(query, context);
    String storage_uuid = UUIDHelpers::UUIDToString(storage->getStorageUUID());
    auto current_topology_version = PairInt64{1, 1};
    cache_manager->mayUpdateTableMeta(*storage, current_topology_version);
    TableMetaEntryPtr entry = cache_manager->getTableMeta(storage->getStorageUUID());
    auto it_p0 = entry->partitions.emplace("1000", std::make_shared<CnchPartitionInfo>(UUIDHelpers::UUIDToString(storage->getStorageUUID()), nullptr, "1000")).first;

    // THREAD 1                             THREAD 2
    // 1.1 try load parts into cache.
    //                                      2.1 try load parts too.
    // 1.2 timeout
    //                                      2.2 Must return valid cache.


    tds.emplace_back([cache_manager, storage, current_topology_version] {
        auto load_func = [](const Strings &, const Strings &) -> DataModelPartWithNameVector {
            std::this_thread::sleep_for(100ms);
            throw Poco::Exception("");
        };
        EXPECT_THROW(
            {
                ServerDataPartsVector res = cache_manager->getOrSetServerDataPartsInPartitions(
                    *storage, {"1000", "1001"}, load_func, TxnTimestamp::maxTS(), current_topology_version);
            },
            Poco::Exception);
    });

    tds.emplace_back([cache_manager, storage, current_topology_version] {
        std::this_thread::sleep_for(50ms);
        auto load_func = [](const Strings &, const Strings &) -> DataModelPartWithNameVector {
            std::this_thread::sleep_for(100ms);
            return CacheTestMock::createPartsBatch("1000", 10);
        };

        ServerDataPartsVector res = cache_manager->getOrSetServerDataPartsInPartitions(
                    *storage, {"1000", "1001"}, load_func, TxnTimestamp::maxTS(), current_topology_version);
        EXPECT_EQ(res.size(), 10);
    });

    for (auto & td : tds)
    {
        td.join();
    }

    cache_manager->shutDown();
}

TEST_F(CacheManagerTest, AsyncReset) {
    std::shared_ptr<PartCacheManager> cache_manager = std::make_shared<PartCacheManager>(getContext().context, true);
    String query = "create table gztest.test UUID '61f0c404-5cb3-11e7-907b-a6006ad3dba0' (id Int32) ENGINE=CnchMergeTree order by id";
    StoragePtr storage = CacheTestMock::createTable(query, getContext().context);
    auto current_topology_version = PairInt64{1, 1};

    EXPECT_EQ(cache_manager->cleanTrashedActiveTables(), 0);
    EXPECT_EQ(cache_manager->getAllActiveTables().size(), 0);

    cache_manager->reset();


    EXPECT_EQ(cache_manager->cleanTrashedActiveTables(), 0);
    EXPECT_EQ(cache_manager->getAllActiveTables().size(), 0);

    cache_manager->mayUpdateTableMeta(*storage, current_topology_version);
    TableMetaEntryPtr entry = cache_manager->getTableMeta(storage->getStorageUUID());
    auto it_p0 = entry->partitions.emplace("1000", std::make_shared<CnchPartitionInfo>(UUIDHelpers::UUIDToString(storage->getStorageUUID()), nullptr, "1000")).first;

    EXPECT_EQ(cache_manager->getAllActiveTables().size(), 1);

    cache_manager->reset();

    EXPECT_EQ(cache_manager->cleanTrashedActiveTables(), 1);

    EXPECT_EQ(cache_manager->getAllActiveTables().size(), 0);

    cache_manager->shutDown();
}

TEST_F(CacheManagerTest, RawStorageCacheRenameTest)
{
    // init storage cache
    auto context = getContext().context;
    CnchStorageCachePtr storageCachePtr = std::make_shared<CnchStorageCache>(1000);

    String create_query_1 = "create table db_test.test UUID '00000000-0000-0000-0000-000000000001' (id Int32) ENGINE=CnchMergeTree order by id";
    String create_query_2 = "create table db_test.test UUID '00000000-0000-0000-0000-000000000002' (id Int32) ENGINE=CnchMergeTree order by id";

    StoragePtr storage1 = CacheTestMock::createTable(create_query_1, context);
    StoragePtr storage2 = CacheTestMock::createTable(create_query_2, context);
    StoragePtr get_by_name = nullptr, get_by_uuid = nullptr;

    // test insert into storage cache and get
    storageCachePtr->insert(storage1->getStorageID(), TxnTimestamp{UInt64{1}}, storage1);
    get_by_name = storageCachePtr->get("db_test", "test");
    get_by_uuid = storageCachePtr->get(UUIDHelpers::toUUID("00000000-0000-0000-0000-000000000001"));
    EXPECT_NE(get_by_name, nullptr);
    EXPECT_NE(get_by_uuid, nullptr);
    EXPECT_EQ(get_by_name->getStorageUUID(), UUIDHelpers::toUUID("00000000-0000-0000-0000-000000000001"));
    EXPECT_EQ(get_by_uuid->getStorageID().database_name, "db_test");
    EXPECT_EQ(get_by_uuid->getStorageID().table_name, "test");

    // test insert storage with different uuid but the same table name (mock rename).
    storageCachePtr->insert(storage2->getStorageID(), TxnTimestamp{UInt64{2}}, storage2);
    get_by_name = storageCachePtr->get("db_test", "test");
    get_by_uuid = storageCachePtr->get(UUIDHelpers::toUUID("00000000-0000-0000-0000-000000000002"));
    EXPECT_NE(get_by_name, nullptr);
    EXPECT_NE(get_by_uuid, nullptr);
    EXPECT_EQ(get_by_name->getStorageUUID(), UUIDHelpers::toUUID("00000000-0000-0000-0000-000000000002"));
    EXPECT_EQ(get_by_uuid->getStorageID().database_name, "db_test");
    EXPECT_EQ(get_by_uuid->getStorageID().table_name, "test");

    // test insert old storage again. should fail to update cache.
    storageCachePtr->insert(storage1->getStorageID(), TxnTimestamp{UInt64{1}}, storage1);
    get_by_name = storageCachePtr->get("db_test", "test");
    get_by_uuid = storageCachePtr->get(UUIDHelpers::toUUID("00000000-0000-0000-0000-000000000001"));
    EXPECT_EQ(get_by_uuid, nullptr);
    EXPECT_NE(get_by_name, nullptr);
    EXPECT_EQ(get_by_name->getStorageUUID(), UUIDHelpers::toUUID("00000000-0000-0000-0000-000000000002"));
}
