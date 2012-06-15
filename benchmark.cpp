#ifndef MONGO_EXPOSE_MACROS
# define MONGO_EXPOSE_MACROS
#endif

#include <mongo/client/dbclient.h>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/signals2/mutex.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>

#ifndef _WIN32
#include <cxxabi.h>
#endif

using namespace std;
using namespace mongo;


namespace {
    const int thread_nums[] = {10, 50, 75, 100, 175, 250, 500, 1000};
    const int max_threads = 1000;
    // Global connections
    DBClientConnection _conn[max_threads];

    bool multi_db = false;

    const char* _db = "foursquare";
    const char* _coll = "users";
    string ns[max_threads];


    // wrapper funcs to route to different dbs. thread == -1 means all dbs
    void ensureIndex(int thread, const BSONObj& obj) {
        if (!multi_db){
            _conn[max(0,thread)].resetIndexCache();
            _conn[max(0,thread)].ensureIndex(ns[0], obj);
        }
        else if (thread != -1){
            _conn[thread].resetIndexCache();
            _conn[thread].ensureIndex(ns[thread], obj);
            return;
        }
        else {
            for (int t=0; t<max_threads; t++) {
                ensureIndex(t, obj);
            }
        }
    }

    template <typename VectorOrBSONObj>
    void insert(int thread, const VectorOrBSONObj& obj) {
        if (!multi_db){
            _conn[max(0,thread)].insert(ns[0], obj);
        }
        else if (thread != -1){
            _conn[thread].insert(ns[thread], obj);
            return;
        }
        else {
            for (int t=0; t<max_threads; t++) {
                insert(t, obj);
            }
        }
    }

    void update(int thread, const BSONObj& qObj, const BSONObj uObj, bool upsert=false, bool multi=false) {
        assert(thread != -1); // cant run on all conns
        _conn[thread].update(ns[multi_db?thread:0], qObj, uObj, upsert, multi);
        return;
    }

    void findOne(int thread, const BSONObj& obj) {
        assert(thread != -1); // cant run on all conns
        _conn[thread].findOne(ns[multi_db?thread:0], obj);
        return;
    }

    auto_ptr<DBClientCursor> query(int thread, const Query& q, int limit=0, int skip=0) {
        assert(thread != -1); // cant run on all conns
        return _conn[thread].query(ns[multi_db?thread:0], q, limit, skip);
    }

    void getLastError(int thread=-1) {
        if (thread != -1){
            _conn[thread].getLastError();
            return;
        }

        for (int t=0; t<max_threads; t++)
            getLastError(t);
    }


    // passed in as argument
    int seconds;
    // protect iterations with _mutex
    boost::signals2::mutex _mutex;
    int iterations;

    struct TestBase{
        virtual void run(int threadId, int seconds) = 0;
        virtual void reset() = 0;
        virtual string name() = 0;
        virtual ~TestBase() {}
    };

    template <typename T>
    struct Test: TestBase{
        virtual void run(int threadId, int seconds) {
            test.run(threadId, seconds);
            getLastError(threadId); //wait for operation to complete
        }
        virtual void reset(){
            test.reset();
        }

        virtual string name(){
            //from mongo::regression::demangleName()
#ifdef _WIN32
            return typeid(T).name();
#else
            int status;

            char * niceName = abi::__cxa_demangle(typeid(T).name(), 0, 0, &status);
            if ( ! niceName )
                return typeid(T).name();

            string s = niceName;
            free(niceName);
            return s;
#endif
        }

        T test;
    };

    struct TestSuite{
            template <typename T>
            void add(){
                tests.push_back(new Test<T>());
            }
            void run(){
                for (vector<TestBase*>::iterator it=tests.begin(), end=tests.end(); it != end; ++it){
                    TestBase* test = *it;
                    boost::posix_time::ptime startTime, endTime; //reused

                    cerr << "########## " << test->name() << " ##########" << endl;

                    BSONObjBuilder results;

                    double one_micros;
                    iterations = 0;
                    BOOST_FOREACH(int nthreads, thread_nums){
                        test->reset();
                        startTime = boost::posix_time::microsec_clock::universal_time();
                        launch_subthreads(nthreads, test, seconds);
                        endTime = boost::posix_time::microsec_clock::universal_time();
                        double micros = (endTime-startTime).total_microseconds() / 1000000.0;

                        if (nthreads == 1)
                            one_micros = micros;

                        results.append(BSONObjBuilder::numStr(nthreads),
                                       BSON( "time" << micros
                                          << "ops" << iterations
                                          << "ops_per_sec" << iterations / micros
                                          << "speedup" << one_micros / micros
                                          ));
                    }

                    BSONObj out =
                        BSON( "name" << test->name()
                           << "results" << results.obj()
                           );
                    cout << out.jsonString(Strict) << endl;
                }
            }
        private:
            vector<TestBase*> tests;

            void launch_subthreads(int threadId, TestBase* test, int seconds) {
                if (!threadId) return;

                boost::thread athread(boost::bind(&TestBase::run, test, threadId, seconds));

                launch_subthreads(threadId - 1, test, seconds);

                athread.join();
            }
    };

/*
    void clearDB(){
        for (int i=0; i<max_threads; i++) {
            _conn[0].dropDatabase(_db + BSONObjBuilder::numStr(i));
            _conn[0].getLastError();
            if (!multi_db)
                return;
        }
    }
    */
}

/*
namespace Overhead{
    // this tests the overhead of the system
    struct DoNothing{
        void run(int t, int n) {}
        void reset(){ clearDB(); }
    };
}

namespace Insert{
    struct Base{
        void reset(){ clearDB(); }
    };

    struct Empty : Base{
        void run(int t, int n) {
            for (int i=0; i < seconds / n; i++){
                insert(t, BSONObj());
            }
        }
    };

    template <int BatchSize>
    struct EmptyBatched : Base{
        void run(int t, int n) {
            for (int i=0; i < seconds / BatchSize / n; i++){
                vector<BSONObj> objs(BatchSize);
                insert(t, objs);
            }
        }
    };

    struct EmptyCapped : Base{
        void run(int t, int n) {
            for (int i=0; i < seconds / n; i++){
                insert(t, BSONObj());
            }
        }
        void reset(){
            clearDB();
            for (int t=0; t<max_threads; t++){
                _conn[t].createCollection(ns[t], 32 * 1024, true);
                if (!multi_db)
                    return;
            }
        }
    };

    struct JustID : Base{
        void run(int t, int n) {
            for (int i=0; i < seconds / n; i++){
                BSONObjBuilder b;
                b << GENOID;
                insert(t, b.obj());
            }
        }
    };

    struct IntID : Base{
        void run(int t, int n) {
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                insert(t, BSON("_id" << base + i));
            }
        }
    };

    struct IntIDUpsert : Base{
        void run(int t, int n) {
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                update(t, BSON("_id" << base + i), BSONObj(), true);
            }
        }
    };

    struct JustNum : Base{
        void run(int t, int n) {
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                insert(t, BSON("x" << base + i));
            }
        }
    };

    struct JustNumIndexedBefore : Base{
        void run(int t, int n) {
            ensureIndex(t, BSON("x" << 1));
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                insert(t, BSON("x" << base + i));
            }
        }
    };

    struct JustNumIndexedAfter : Base{
        void run(int t, int n) {
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                insert(t, BSON("x" << base + i));
            }
            ensureIndex(t, BSON("x" << 1));
        }
    };

    struct NumAndID : Base{
        void run(int t, int n) {
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                BSONObjBuilder b;
                b << GENOID;
                b << "x" << base+i;
                insert(t, b.obj());
            }
        }
    };
}

namespace Update{
    struct Base{
        void reset(){ clearDB(); }
    };

    struct IncNoIndexUpsert : Base{
        void run(int t, int n) {
            const int incs = seconds/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    update(t, BSON("_id" << i), BSON("$inc" << BSON("count" << 1)), 1);
                }
            }
        }
    };
    struct IncWithIndexUpsert : Base{
        void reset(){ clearDB(); ensureIndex(-1, BSON("count" << 1));}
        void run(int t, int n) {
            const int incs = seconds/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    update(t, BSON("_id" << i), BSON("$inc" << BSON("count" << 1)), 1);
                }
            }
        }
    };
    struct IncNoIndex : Base{
        void reset(){
            clearDB();
            for (int i=0; i<100; i++)
                insert(-1, BSON("_id" << i << "count" << 0));
        }
        void run(int t, int n) {
            const int incs = seconds/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    update(t, BSON("_id" << i), BSON("$inc" << BSON("count" << 1)));
                }
            }
        }
    };
    struct IncWithIndex : Base{
        void reset(){
            clearDB();
            ensureIndex(-1, BSON("count" << 1));
            for (int i=0; i<100; i++)
                insert(-1, BSON("_id" << i << "count" << 0));
        }
        void run(int t, int n) {
            const int incs = seconds/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    update(t, BSON("_id" << i), BSON("$inc" << BSON("count" << 1)));
                }
            }
        }
    };
    struct IncNoIndex_QueryOnSecondary : Base{
        void reset(){
            clearDB();
            ensureIndex(-1, BSON("i" << 1));
            for (int i=0; i<100; i++)
                insert(-1, BSON("_id" << i << "i" << i << "count" << 0));
        }
        void run(int t, int n) {
            const int incs = seconds/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    update(t, BSON("i" << i), BSON("$inc" << BSON("count" << 1)));
                }
            }
        }
    };
    struct IncWithIndex_QueryOnSecondary : Base{
        void reset(){
            clearDB();
            ensureIndex(-1, BSON("count" << 1));
            ensureIndex(-1, BSON("i" << 1));
            for (int i=0; i<100; i++)
                insert(-1, BSON("_id" << i << "i" << i << "count" << 0));
        }
        void run(int t, int n) {
            const int incs = seconds/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    update(t, BSON("i" << i), BSON("$inc" << BSON("count" << 1)));
                }
            }
        }
    };
}

namespace Queries{
    struct Empty{
        void reset() {
            clearDB();
            for (int i=0; i < seconds; i++){
                insert(-1, BSONObj());
            }
            getLastError();
        }

        void run(int t, int n){
            int chunk = seconds / n;
            auto_ptr<DBClientCursor> cursor = query(t, BSONObj(), chunk, chunk*t);
            cursor->itcount();
        }
    };

    struct HundredTableScans{
        void reset() {
            clearDB();
            for (int i=0; i < seconds; i++){
                insert(-1, BSONObj());
            }
            getLastError();
        }

        void run(int t, int n){
            for (int i=0; i < 100/n; i++){
                findOne(t, BSON("does_not_exist" << i));
            }
        }
    };

    struct IntID{
        void reset() {
            clearDB();
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("_id" << i));
            }
            getLastError();
        }

        void run(int t, int n){
            int chunk = seconds / n;
            auto_ptr<DBClientCursor> cursor = query(t, BSONObj(), chunk, chunk*t);
            cursor->itcount();
        }
    };

    struct IntIDRange{
        void reset() {
            clearDB();
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("_id" << i));
            }
            getLastError();
        }

        void run(int t, int n){
            int chunk = seconds / n;
            auto_ptr<DBClientCursor> cursor = query(t, BSON("_id" << GTE << chunk*t << LT << chunk*(t+1)));
            cursor->itcount();
        }
    };

    struct IntIDFindOne{
        void reset() {
            clearDB();
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("_id" << i));
            }
            getLastError();
        }

        void run(int t, int n){
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                findOne(t, BSON("_id" << base + i));
            }
        }
    };

    struct IntNonID{
        void reset() {
            clearDB();
            ensureIndex(-1, BSON("x" << 1));
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("x" << i));
            }
            getLastError();
        }

        void run(int t, int n){
            int chunk = seconds / n;
            auto_ptr<DBClientCursor> cursor = query(t, BSONObj(), chunk, chunk*t);
            cursor->itcount();
        }
    };

    struct IntNonIDRange{
        void reset() {
            clearDB();
            ensureIndex(-1, BSON("x" << 1));
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("x" << i));
            }
            getLastError();
        }

        void run(int t, int n){
            int chunk = seconds / n;
            auto_ptr<DBClientCursor> cursor = query(t, BSON("x" << GTE << chunk*t << LT << chunk*(t+1)));
            cursor->itcount();
        }
    };

    struct IntNonIDFindOne{
        void reset() {
            clearDB();
            ensureIndex(-1, BSON("x" << 1));
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("x" << i));
            }
            getLastError();
        }

        void run(int t, int n){
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                findOne(t, BSON("x" << base + i));
            }
        }
    };

    struct RegexPrefixFindOne{
        RegexPrefixFindOne(){
            for (int i=0; i<100; i++)
                nums[i] = "^" + BSONObjBuilder::numStr(i+1);
        }
        void reset() {
            clearDB();
            ensureIndex(-1, BSON("x" << 1));
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("x" << BSONObjBuilder::numStr(i)));
            }
            getLastError();
        }

        void run(int t, int n){
            for (int i=0; i < seconds / n / 100; i++){
                for (int j=0; j<100; j++){
                    BSONObjBuilder b;
                    b.appendRegex("x", nums[j]);
                    findOne(t, b.obj());
                }
            }
        }
        string nums[100];
    };

    struct TwoIntsBothGood{
        void reset() {
            clearDB();
            ensureIndex(-1, BSON("x" << 1));
            ensureIndex(-1, BSON("y" << 1));
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("x" << i << "y" << (seconds-i)));
            }
            getLastError();
        }

        void run(int t, int n){
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                findOne(t, BSON("x" << base + i << "y" << (seconds-(base+i))));
            }
        }
    };

    struct TwoIntsFirstGood{
        void reset() {
            clearDB();
            ensureIndex(-1, BSON("x" << 1));
            ensureIndex(-1, BSON("y" << 1));
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("x" << i << "y" << (i%13)));
            }
            getLastError();
        }

        void run(int t, int n){
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                findOne(t, BSON("x" << base + i << "y" << ((base+i)%13)));
            }
        }
    };

    struct TwoIntsSecondGood{
        void reset() {
            clearDB();
            ensureIndex(-1, BSON("x" << 1));
            ensureIndex(-1, BSON("y" << 1));
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("x" << (i%13) << "y" << i));
            }
            getLastError();
        }

        void run(int t, int n){
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                findOne(t, BSON("x" << ((base+i)%13) << "y" << base+i));
            }
        }
    };
    struct TwoIntsBothBad{
        void reset() {
            clearDB();
            ensureIndex(-1, BSON("x" << 1));
            ensureIndex(-1, BSON("y" << 1));
            for (int i=0; i < seconds; i++){
                insert(-1, BSON("x" << (i%503) << "y" << (i%509))); // both are prime
            }
            getLastError();
        }

        void run(int t, int n){
            int base = t * (seconds/n);
            for (int i=0; i < seconds / n; i++){
                findOne(t, BSON("x" << ((base+i)%503) << "y" << ((base+i)%509)));
            }
        }
    };

}
*/

namespace FSTests {
    struct LookupUserByID {
        void reset() { }

        void run(int threadId, int seconds){
            boost::posix_time::ptime startTime = boost::posix_time::microsec_clock::universal_time();
            boost::posix_time::ptime endTime = startTime + boost::posix_time::seconds(seconds);
            int iters = 0;
            while (boost::posix_time::microsec_clock::universal_time() < endTime) {
                findOne(threadId, BSON("_id" << threadId + iters));
                ++iters;
            }
            {
              boost::interprocess::scoped_lock<boost::signals2::mutex> lk(_mutex);
              iterations += iters;
            }
        }
    };
}

namespace{
    struct TheTestSuite : TestSuite{
        TheTestSuite(){
          add<FSTests::LookupUserByID>();
        /*
            //add< Overhead::DoNothing >();

            add< Insert::Empty >();
            add< Insert::EmptyBatched<2> >();
            add< Insert::EmptyBatched<10> >();
            //add< Insert::EmptyBatched<100> >();
            //add< Insert::EmptyBatched<1000> >();
            //add< Insert::EmptyCapped >();
            //add< Insert::JustID >();
            add< Insert::IntID >();
            add< Insert::IntIDUpsert >();
            //add< Insert::JustNum >();
            add< Insert::JustNumIndexedBefore >();
            add< Insert::JustNumIndexedAfter >();
            //add< Insert::NumAndID >();

            add< Update::IncNoIndexUpsert >();
            add< Update::IncWithIndexUpsert >();
            add< Update::IncNoIndex >();
            add< Update::IncWithIndex >();
            add< Update::IncNoIndex_QueryOnSecondary >();
            add< Update::IncWithIndex_QueryOnSecondary >();

            //add< Queries::Empty >();
            add< Queries::HundredTableScans >();
            //add< Queries::IntID >();
            add< Queries::IntIDRange >();
            add< Queries::IntIDFindOne >();
            //add< Queries::IntNonID >();
            add< Queries::IntNonIDRange >();
            add< Queries::IntNonIDFindOne >();
            //add< Queries::RegexPrefixFindOne >();
            //add< Queries::TwoIntsBothBad >();
            //add< Queries::TwoIntsBothGood >();
            //add< Queries::TwoIntsFirstGood >();
            //add< Queries::TwoIntsSecondGood >();
       */
        }
    } theTestSuite;
}

int main(int argc, const char **argv){
    if (argc < 3){
        cout << argv[0] << " [host:port] [seconds] [multidb (1 or 0)]" << endl;
        return 1;
    }

    for (int i=0; i < max_threads; i++){
        ns[i] = _db + BSONObjBuilder::numStr(i) + '.' + _coll;
        string errmsg;
        if ( ! _conn[i].connect( string( argv[1] ), errmsg ) ) {
            cout << "couldn't connect : " << errmsg << endl;
            return 1;
        }
    }

    seconds = atoi(argv[2]);

    if (argc > 3)
        multi_db = (argv[3][0] == '1');

    theTestSuite.run();

    return 0;
}
