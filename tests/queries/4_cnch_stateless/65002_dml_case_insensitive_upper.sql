set text_case_option='UPPERCASE';
set dialect_type='MYSQL';
CREATE DATABASE IF NOT EXISTS TEST_upperCASE1;
use TEST_upperCASE1;
DROP TABLE IF EXISTS test_insert_CASE1;

create table test_insert_CASE1 (
    EVENT_date Date,
    EVENT_TYPE String,
    EVENT_COUNT Int32,
    EventDate Date DEFAULT toDate(EVENT_date)
) engine = CnchMergeTree()
partition by toYYYYMM(EVENT_DATE)
order by (EVENT_type, EVENT_count);

INSERT INTO TEST_upperCASE1.test_INSERT_CASE1 (EVENT_DATE, EVENT_TYPE, EVENT_COUNT) VALUES ('2022-01-01', 'Type1', 10);

INSERT INTO TEST_upperCASE1.test_INSERT_CASE1 (EVENT_DATE, EVENT_TYPE, EVENT_COUNT) VALUES 
('2022-01-02', 'Type2', 20), ('2022-01-03', 'Type3', 30);

INSERT INTO TEST_upperCASE1.test_INSERT_CASE1 (EVENT_DATE, EVENT_TYPE, EVENT_COUNT)
SELECT EVENT_date, EVENT_TYPE, EVENT_COUNT FROM TEST_upperCASE1.test_INSERT_CASE1 WHERE EVENT_DATE = '2022-01-01' ORDER BY EVENT_COUNT;

INSERT INTO TEST_upperCASE1.test_INSERT_CASE1 (EVENT_TYPE, EVENT_COUNT) VALUES ('Type4', 40);

SELECT * FROM TEST_upperCASE1.test_INSERT_CASE1 ORDER BY EVENT_COUNT;

DROP TABLE test_insert_CASE1;
DROP DATABASE TEST_upperCASE1;


Drop daTABaSE IF ExiSTS uPdate_SeT_upper1;
cREaTe datAbaSE uPdate_SeT_upper1;
creATe Table uPdate_SeT_upper1.COmMON_TaBle(
    `id` Int NOT NULL,
    `STRINg` vaRCHAr(30),
    `dATE` DATe,
    `DATeTIme` daTEtImE,
    `FlOat` FLOat,
    `DOUBLE` DoUblE
)EnginE=CnchMergeTree
OrDEr bY ID
PriMaRY key ID
unique key id
paRTitION BY dAte;

UpDAtE uPdate_SeT_upper1.CoMMon_TablE sEt stRINg='tODaYISmoNDAY' wHEre ID < 10 OrDeR bY ID limit 3;

DROP DATABASE uPdate_SeT_upper1;
