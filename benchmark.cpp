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
    const int thread_nums[] = {10, 50, 75, 100, 175, 250, 500};
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

    void queryAndExhaustCursor(int thread, const Query& q, int limit=0, int skip=0) {
        auto_ptr<DBClientCursor> cur = query(thread, q, limit, skip);
        while (cur->more()) {
          cur->nextSafe();
        }
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
                        double micros = (endTime-startTime).total_microseconds() / 1000001.0;

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

static int userids[] = {
  19455489, 5381772, 5701820, 5754058, 10876381, 5279698, 6130363, 6150116, 19368260, 19457933, 19457344,
  19461626, 19460087, 19468439, 19471498, 19480617, 19488894, 19534335, 19542684, 19584682, 19642128, 1865666,
  1851367, 5996532, 5139385, 5156629, 5180497, 5193466, 5313793, 5368017, 5374069, 5777830, 5517409, 5543195,
  5631987, 5749877, 5875052, 5960534, 6014078, 6032225, 6033697, 6042999, 6054869, 6056953, 6061721, 6152341,
  10930518, 10815503, 5305542, 5656610, 6097200, 19262825, 19430530, 19460833, 19484165, 19486131, 19486216,
  19470329, 19473434, 19477845, 19484340, 19468227, 19491082, 19465018, 19475210, 19480370, 19484887, 19481308,
  19524071, 19525107, 19523044, 19549779, 19566789, 19571543, 19591001, 19565545, 19622313, 19623234, 19607744,
  19636836, 1862285, 703394, 762510, 761230, 5964600, 5272819, 5451372, 5993616, 5553594, 5743879, 5930071,
  6078142, 5626619, 5262911, 5080634, 5109133, 5145545, 5175549, 5183400, 5202099, 5223949, 5293845, 5310342,
  5313953, 5314923, 5346842, 5365646, 5382992, 5412741, 5414067, 5460185, 5466015, 5507624, 5552910, 5556613,
  5566570, 5581125, 5611254, 5616810, 5627512, 5659487, 5665995, 5687388, 5698060, 5705066, 5716321, 5738754,
  5747974, 5773013, 5782029, 5783917, 5809119, 5809598, 5830369, 5841450, 5850175, 5855878, 5867289, 5873944,
  5875127, 5882234, 5912046, 5928831, 5949513, 5967876, 5973757, 5977662, 6011335, 6016486, 6037350, 6048204,
  6090022, 6109023, 6152350, 10842433, 10894876, 5131479, 5589640, 5801160, 5947009, 6151093, 10812700, 10847949,
  10848106, 10875217, 14209473, 6016112, 5297228, 5168087, 5205598, 5483321, 5252743, 5659936, 5697111, 5964392,
  6126729, 6129525, 6147259, 6150967, 19279125, 19316933, 19408461, 19456273, 19430614, 19457846, 19461055,
  19460472, 19459783, 19459820, 19460960, 19472964, 19489794, 19471851, 19476728, 19486590, 19483711, 19483504,
  19480996, 19490337, 19486815, 19486720, 19490134, 19490824, 19465071, 19486503, 19486787, 19483645, 19489801,
  19475499, 19467145, 19465930, 19463588, 19463753, 19470702, 19470827, 19467670, 19476429, 19474585, 19480254,
  19479397, 19478401, 19477745, 19483573, 19482084, 19482118, 19481681, 19481342, 19487175, 19492168, 19491262,
  19489820, 19489266, 19524500, 19522675, 19498000, 19494315, 19513811, 19517599, 19506799, 19504166, 19556622,
  19550440, 19539017, 19555176, 19554888, 19557716, 19531969, 19561003, 19586202, 19590474, 19564687, 19570542,
  19565195, 19587240, 19582965, 19591341, 19581870, 19600906, 19599117, 19605308, 19618747, 19622193, 19593570,
  19622209, 19607217, 19623350, 19622821, 19625311, 19598992, 19653377, 19626978, 19632192, 19639692, 19649172,
  19640637, 19631821, 19632736, 19632260, 19651459, 19649540, 19655146, 19654927, 19652514, 19629888, 12658,
  1859137, 1866772, 1867627, 1850421, 2195431, 2196642, 19595500, 698977, 694913, 696531, 707688, 699189, 699224,
  748804, 766384, 754784, 765009, 779270, 778280, 6128178, 14199358, 5424025, 5500389, 5345470, 6136177, 6072626,
  5374094, 5348327, 5821083, 5829650, 5797758, 5770501, 5387479, 10847616, 6025659, 10869646, 5792764, 5331224,
  5797735, 10845767, 5159803, 5861312, 6103909, 5708422, 5291410, 5075820, 10870196, 5378241, 6075888, 6051796,
  5860247, 5261310, 5762915, 5845890, 5531363, 5981447, 5157842, 5758956, 5846161, 5077832, 5079905, 5080189,
  5091536, 5092691, 5099202, 5101167, 5119235, 5145209, 5152923, 5158316, 5159709, 5160853, 5161458, 5171809,
  5172973, 5176275, 5184879, 5194030, 5199809, 5201479, 5202181, 5203406, 5209544, 5215952, 5216860, 5217643,
  5227613, 5230150, 5231773, 5233890, 5244923, 5246361, 5260515, 5263298, 5269271, 5273219, 5276604, 5280951,
  5281704, 5283805, 5286734, 5291790, 5293197, 5294738, 5304232, 5307462, 5316174, 5319682, 5325920, 5339657,
  5340013, 5340067, 5347366, 5355592, 5356845, 5359302, 5359982, 5360235, 5360326, 5360922, 5361339, 5367215,
  5368315, 5371742, 5376226, 5377020, 5383694, 5383890, 5384829, 5390736, 5394298, 5395190, 5395193, 5397611,
  5410450, 5410628, 5411635, 5581130, 5571493, 5474582, 6036667, 5711331, 5464844, 6110302, 5693510, 5417294,
  5418586, 5418923, 5424500, 5425088, 5427567, 5433777, 5439533, 5440047, 5440870, 5447028, 5455880, 5458959,
  5463818, 5464380, 5468671, 5468868, 5469239, 5473695, 5475999, 5478022, 5489925, 5497698, 5500647, 5500791,
  5502899, 5502981, 5503109, 5506956, 5515654, 5525436, 5526007, 5527024, 5528163, 5528389, 5542513, 5547452,
  5549007, 5551816, 5567258, 5572753, 5576758, 5580048, 5580553, 5583182, 5584008, 5585161, 5587541, 5588780,
  5588982, 5592457, 5598318, 5603369, 5605317, 5607160, 5615687, 5616842, 5617707, 5620679, 5622710, 5622867,
  5625267, 5628717, 5849973, 5634856, 5634955, 5638090, 5638270, 5638638, 5641407, 5641734, 5650178, 5650592,
  5650601, 5661448, 5673059, 5674431, 5676038, 5680060, 5683914, 5689146, 5689561, 5695223, 5698031, 5700595,
  5703525, 5705728, 5710803, 5714034, 5714108, 5719732, 5722924, 5732598, 5758643, 5761240, 5766655, 5774088,
  5777134, 5779938, 5781723, 5783896, 5795937, 5796155, 5807080, 5808053, 5808930, 5817158, 5828844, 5829696,
  5833765, 5835921, 5838918, 5843668, 5844483, 5848407, 5852020, 5858103, 5863974, 5866029, 5870436, 5875148,
  5876798, 5882319, 5883883, 5884517, 5894257, 5897300, 5898649, 5911280, 5912611, 5914672, 5923499, 5924194,
  5925854, 5929074, 5940136, 5947160, 5950297, 5951659, 5952243, 5958883, 5960304, 5965195, 5979799, 5980142,
  5985731, 5987986, 5999497, 6002814, 6004400, 6006597, 6011030, 6011338, 6013784, 6030010, 6030791, 6033472,
  6034955, 6036011, 6036254, 6039172, 6042677, 6043349, 6057708, 6064538, 6065159, 6066415, 6066801, 6068909,
  6070412, 6070635, 6072739, 6076617, 6077155, 6077408, 6081856, 6094657, 6096344, 6098361, 6099843, 6099900,
  6103653, 6112307, 6113048, 6114125, 6118547, 6123382, 6125191, 6127991, 6128159, 6128656, 6135000, 6137700,
  6139012, 6142637, 6144525, 6145028, 6148856, 6149251, 6153492, 6166604, 6167666, 6168511, 10804260, 10809332,
  10832518, 10833749, 10839485, 10847163, 10851553, 10893643, 10905933, 10913838, 10920605, 10936869, 5103172,
  5263612, 5567663, 5596004, 5676569, 5707404, 5743537, 5800600, 5919465, 5952010, 5953799, 5955218, 5971574,
  5986624, 6023933, 6042610, 6050102, 6051747, 6068192, 6078700, 6101250, 6149191, 6158449, 6170677, 5760618,
  10807709, 10810905, 10818194, 10821574, 10823756, 10829484, 10832388, 10839435, 10853185, 10861244, 10866240,
  10866370, 10866653, 10872761, 10877002, 10888104, 10890525, 10890886, 10891570, 10893415, 10897313, 10908865,
  10918918, 10928832, 10930187, 10940366, 14208707, 5250473, 10807662, 10891818, 5324310, 5348322, 5409482,
  5229785, 5814678, 10814491, 10819154, 10826159, 10849131, 10840525, 10837297, 10829038, 10878515, 10895638,
  10893234, 10897418, 10910623, 10925127, 5473378, 5087930, 5093806, 5104117, 5137322, 5147662, 5152343, 5186475,
  5196533, 5206699, 5221130, 5249253, 5270882, 5276681, 5308780, 5323710, 5326066, 5338018, 5337515, 5386754,
  5428515, 5427860, 5436370, 5442775, 5471932, 5479092, 5484866, 5510605, 5525956, 5531050, 5560484, 5573746,
  5581493, 5585552, 5605648, 5637731, 5638045, 5650510, 5660604, 5683519, 5692462, 5703468, 5730609, 5781849,
  5781202, 5793186, 5817511, 5849077, 5877373, 5914305, 5920081, 5957880, 5957314, 5972804, 5979165, 5984197,
  5994281, 6012157, 6036466, 6041440, 6075155, 6124029, 6141693, 6167230, 14226276, 15369087, 19245618, 19256981,
  19255374, 19237120, 19256384, 19246328, 19240321, 19245401, 19227382, 19231241, 19231493, 19236009, 19235992,
  19233082, 19233430, 19246627, 19255462, 19269216, 19288270, 19273862, 19259296, 19282374, 19278981, 19326603,
  19322115, 19297717, 19300129, 19311961, 19315198, 19323960, 19363287, 19360840, 19376819, 19380535, 19389149,
  19389213, 19370457, 19397636, 19418406, 19424611, 19425820, 19443798, 19450976, 19455250, 19454097, 19430166,
  19436950, 19455116, 19437150, 19435917, 19433708, 19439617, 19439673, 19438059, 19446292, 19443533, 19442892,
  19448243, 19447150, 19451264, 19458919, 19457822, 19456532, 19456066, 19456255, 19455888, 19462413, 19461979,
  19460587, 19459700, 19451902, 19433627, 19435027, 19442564, 19447017, 19455143, 19430927, 19459719, 19441884,
  19438798, 19448479, 19485623, 19490389, 19493688, 19491155, 19482054, 19482627, 19466365, 19492032, 19490298,
  19479903, 19481758, 19467643, 19487017, 19481192, 19463348, 19467041, 19471363, 19470279, 19469930, 19468929,
  19467534, 19476059, 19473459, 19472502, 19472339, 19480079, 19479657, 19477113, 19488943, 19487842, 19490566,
  19489458, 19478569, 19473530, 19488787, 19493403, 19463129, 19473532, 19480711, 19484105, 19468882, 19471727,
  19485033, 19489164, 19489645, 19483538, 19471110, 19471657, 19481891, 19481417, 19488316, 19481727, 19465226,
  19476474, 19492777, 19493012, 19465585, 19466620, 19464919, 19465983, 19463089, 19479530, 19482735, 19483833,
  19483030, 19481437, 19479541, 19484757, 19472875, 19470075, 19492814, 19486142, 19481725, 19464009, 19474161,
  19466613, 19463391, 19462702, 19467376, 19466872, 19465582, 19465850, 19465155, 19464858, 19463897, 19471112,
  19469741, 19469279, 19469467, 19468020, 19468022, 19475943, 19474502, 19474511, 19474718, 19473540, 19473059,
  19473222, 19472715, 19472047, 19472187, 19472483, 19480045, 19480192, 19479211, 19479264, 19479362, 19477835,
  19477866, 19484580, 19484882, 19484115, 19483928, 19482556, 19482912, 19482316, 19481881, 19481081, 19480788,
  19480952, 19488555, 19488061, 19487531, 19487001, 19487154, 19486636, 19486995, 19486016, 19485677, 19491585,
  19491831, 19491995, 19491387, 19491441, 19490650
};

namespace FSTests {
    struct SimpleTest {
        void reset() { }

        void run(int threadId, int seconds) {
            boost::posix_time::ptime startTime = boost::posix_time::microsec_clock::universal_time();
            boost::posix_time::ptime endTime = startTime + boost::posix_time::seconds(seconds);
            int iters = 0;
            while (boost::posix_time::microsec_clock::universal_time() < endTime) {
                oneIteration(threadId);
                ++iters;
            }
            {
              boost::interprocess::scoped_lock<boost::signals2::mutex> lk(_mutex);
              iterations += iters;
            }
        }

        virtual void oneIteration(int threadId) = 0;
    };

    struct LookupUserByID : SimpleTest {
        virtual void oneIteration(int threadId) {
            findOne(threadId, BSON("_id" << 19455489)); //threadId + iters));
        }
    };

    struct LookupUserByIDs : SimpleTest {
        virtual void oneIteration(int threadId) {
            queryAndExhaustCursor(threadId, BSON("_id" << BSON("$in" << vector<int>(userids, userids + (sizeof(userids) / sizeof(int))))));
        }
    };

    struct LookupUserByIDsNoExhaust : SimpleTest {
        virtual void oneIteration(int threadId) {
            query(threadId, BSON("_id" << BSON("$in" << vector<int>(userids, userids + (sizeof(userids) / sizeof(int))))));
        }
    };

}

namespace{
    struct TheTestSuite : TestSuite{
        TheTestSuite(){
          add<FSTests::LookupUserByID>();
          add<FSTests::LookupUserByIDs>();
          add<FSTests::LookupUserByIDsNoExhaust>();
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
