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
    const int thread_nums[] = {10, 20, 50, 100, 250, 500};
    const int max_threads = 501;
    // Global connections
    DBClientConnection _conn[max_threads];

    bool multi_db = false;

/*
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
    */

    template <typename VectorOrBSONObj>
    void insert(int thread, const string& ns, const VectorOrBSONObj& obj) {
        if (!multi_db){
            _conn[max(0,thread)].insert(ns, obj);
        }
        else if (thread != -1){
            _conn[thread].insert(ns, obj);
            return;
        }
        else {
            for (int t=0; t<max_threads; t++) {
                insert(t, obj);
            }
        }
    }

    void update(int thread, const string& ns, const BSONObj& qObj, const BSONObj uObj, bool upsert=false, bool multi=false) {
        assert(thread != -1); // cant run on all conns
        _conn[thread].update(ns, qObj, uObj, upsert, multi);
        return;
    }

    void findOne(int thread, const string &ns, const BSONObj& obj) {
        assert(thread != -1); // cant run on all conns
        _conn[thread].findOne(ns, obj);
        return;
    }

    auto_ptr<DBClientCursor> query(int thread, const string& ns, const Query& q, int limit=0, int skip=0) {
        assert(thread != -1); // cant run on all conns
        return _conn[thread].query(ns, q, limit, skip);
    }

    void queryAndExhaustCursor(int thread, const string& ns, const Query& q, int limit=0, int skip=0) {
        auto_ptr<DBClientCursor> cur = query(thread, ns, q, limit, skip);
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

OID venueids[] = {
  OID("4e228599483b041af1f8994a"), OID("4e68e178ae60186280adbd11"), OID("4e482388183849317e8df0a8"),
  OID("4c08d4167e3fc928c1e0f082"), OID("4c17fc751436a59323ab8c75"), OID("4e64c6aed164ddd5e6bfe8f1"),
  OID("4c85976b77be76b04b41b776"), OID("4e10727822713f7d7bc81ca6"), OID("4b8c1444f964a5209abc32e3"),
  OID("4bfe5e7fee20ef3b838a3c5e"), OID("4c18007f2cddb7137514831e"), OID("4c6eda32d97fa1438232f1ca"),
  OID("4df40f7fae609e69dd9c111c"), OID("4e006ac17d8beaa1649c4ce8"), OID("4e3b3b1ab61cb577be5a06ec"),
  OID("4c1cb4afb4e62d7f316ddb93"), OID("4b15507ef964a520aeb023e3"), OID("4bfa6d655317a5934196027f"),
  OID("4b330432f964a5200d1725e3"), OID("4df90e321495b7db1b109908"), OID("4dc0172bf7b115b875d850f1"),
  OID("4e3ff4037d8b0e9610894992"), OID("49e39fa6f964a520a2621fe3"), OID("4e252915ae6035bd16f32da1"),
  OID("4be3c4fef07b0f47d4e4f843"), OID("4b744584f964a5208ad12de3"), OID("4bb627766edc76b01b7a301c"),
  OID("4b53e7adf964a52060ae27e3"), OID("4b774467f964a5203c8d2ee3"), OID("4d48113353ebba7a41785bac"),
  OID("4cbf6411ca4aa1cd66cd19b4"), OID("41253f00f964a5204b0c1fe3"), OID("4c73d4c93c26ef3be1d2e6d5"),
  OID("4c1e4000eac020a1105e49c2"), OID("4a55c1f7f964a52046b41fe3"), OID("4b554f0ff964a5205ce127e3"),
  OID("4c7ffb24e602b1f7fd8b857a"), OID("4bfe74f9bf6576b04eafaeb8"), OID("4e5b973c813034cf45e42a5f"),
  OID("4b994c11f964a520057135e3"), OID("4cb48d14cbab236af126bf73"), OID("4d6cc24540fc8eec87f89eba"),
  OID("4bc4aee4f8219c74cbb7b710"), OID("4d13aa9d2b1fa35dca8c4ab1"), OID("4e15eebad22d026fa772f42c"),
  OID("4b12c9c5f964a5202f8e23e3"), OID("4b65e495f964a52097062be3"), OID("4c5727cb7329c9286d398e80"),
  OID("4d163c98b15cb1f79ffdac21"), OID("4d4c3db22d0d8cfac470cd25"), OID("4d3ac39ecc48224b7f46404f"),
  OID("4b01aa68f964a520664422e3"), OID("4de9fae01f6e3ddebdbf7d69"), OID("4c13e54c7f7f2d7f4cf1df68"),
  OID("4c1913f24ff90f4788610f49"), OID("4d2c488c342d6dcb06b71dcb"), OID("4b058700f964a5208b7a22e3"),
  OID("4e880121f5b9fed680cd87bd"), OID("4b6303a7f964a520b55d2ae3"), OID("4cf85031e8e254817bdfd662"),
  OID("4c82749374d7b60cea4880d8"), OID("4dd381bd2271d21ebd3e0502"), OID("4b5672d5f964a520d11028e3"),
  OID("4b0a8824f964a520dd2423e3"), OID("4baeae08f964a52020ce3be3"), OID("4e5eefc618a870f60f3386f7"),
  OID("4c7c2aa720bb199c20292329"), OID("4b2b4ca4f964a520a2b524e3"), OID("4b5b47caf964a5200cf128e3"),
  OID("4dea3248d164ef597ce39c03"), OID("4a406f30f964a52098a41fe3"), OID("4bcfd6d741b9ef3b9341f9e5"),
  OID("3fd66200f964a520bbe61ee3"), OID("4c5005931886c9b6dce0fa27"), OID("4deaae1f88774880e323a487"),
  OID("4e8db778d3e3720da3fbdb79"), OID("4ba5027af964a520eccf38e3"), OID("4b8827b5f964a52093e331e3"),
  OID("4b44dbfcf964a52078fe25e3"), OID("4b9d90d4f964a520feb336e3"), OID("4c129682a5eb76b0ec74beb7"),
  OID("4ce1c61c00166ea8223a4388"), OID("4c893b24ed0aa1439c0bb0f3"), OID("4e7f366a9a52594cdf7897ec"),
  OID("4e7c497b45ddcbc91eac15a7"), OID("4be79e6de1b39521b41e21c1"), OID("4d46f1f01911a0932391f0d8"),
  OID("4e56be3d18a81fe3e5f1aeb9"), OID("4e252a23d22df6f888772101"), OID("4cc0b364083a9c74fba73678"),
  OID("4c24996ec11dc9b6cfa52524"), OID("4d8d930c3bfef04de3638486"), OID("4b3d7bcaf964a520c69425e3"),
  OID("4b62e276f964a52040572ae3"), OID("4c0f0c667189c9283699d9b6"), OID("4b0588dff964a520d8dd22e3"),
  OID("4b995fd0f964a520ae7635e3"), OID("4b4be91ef964a52010ab26e3"), OID("4d9e033da1ec8cfa95f3114a"),
  OID("4c36129e3849c9289666bbb1"), OID("4c24deeb905a0f4750796060"), OID("4813c340f964a5204b4f1fe3"),
  OID("4b4e7160f964a520eeed26e3"), OID("40b13b00f964a520a2f61ee3"), OID("4b6988f1f964a5204fa62be3"),
  OID("4b7a2519f964a52078242fe3"), OID("4b19839ef964a520acde23e3"), OID("4c98e850671db60c5871bdf6"),
  OID("4ddac24ed22d4dbc8c0bbd22"), OID("4dac2dfd5da3ba8a47b012db"), OID("4a8d54eef964a520760f20e3"),
  OID("4b4b489df964a520709626e3"), OID("4c1b892e624b9c74ba9f1204"), OID("4da88c9381541df437bfd00b"),
  OID("4b64bc16f964a52009cc2ae3"), OID("4cd4cb1c2944b1f7bf7059ec"), OID("4c4dabe251c2c9280016309d"),
  OID("4d92b679b053b60c78857fcb"), OID("439530e7f964a520882b1fe3"), OID("4a997418f964a520672e20e3"),
  OID("4dea287445dd3993a8833862"), OID("4aabf233f964a520195b20e3"), OID("4ba6b36df964a520f46939e3"),
  OID("4caded9a2303a143347feaf0"), OID("4aebfa97f964a5202dc521e3"), OID("4e179c7752b123a586cef176"),
  OID("4b5bebc0f964a5205a1d29e3"), OID("4d31c68f2e56236ab0ef15b4"), OID("4cbb5725dd41a35d889feda0"),
  OID("4c2e63817cc0c9b6a2f7e99a"), OID("4b0a71d7f964a5201a2423e3"), OID("4b6b9d16f964a52094122ce3"),
  OID("4c4b2b88712ac9288ad8896c"), OID("4c378df43849c92828ebbdb1"), OID("4e0cfc16d22d8582bdb11dd3"),
  OID("4a970136f964a520eb2720e3"), OID("4c1be865b4e62d7fdf2eda93"), OID("4a71a12af964a5204ed91fe3"),
  OID("4dcf0b14d4c065592f8f33a9"), OID("4e09fbb518a8382643c5f30c"), OID("4af5edc6f964a520f0fe21e3"),
  OID("4c3c8fcd933b0f479235e421"), OID("4e4276971f6ec93e92549055"), OID("4b8fff9df964a520166f33e3"),
  OID("4c34c4147cc0c9b63ee5f39a"), OID("4b093cd0f964a520db1423e3"), OID("4b4a420df964a520268126e3"),
  OID("4c1be590b306c928fcf862b7"), OID("4b08baf5f964a520861123e3"), OID("4c3cff2817f2ef3bd94281f4"),
  OID("4df4ff9081dc20cdb9af891e"), OID("4ade60ecf964a520747521e3"), OID("4e7e875d77c8c4fb96c4e32a"),
  OID("4e8475b7c2ee9c188c1579a2"), OID("4c3613f6298e9c7497a507e3"), OID("4c14ad9077cea5935ffed060"),
  OID("4bc2829eabf4952159b4c293"), OID("4bed35e29868a593f2045d46"), OID("4c74420966be6dcbb739bd0f"),
  OID("4d99938ac19fb60cee8ec665"), OID("4adcdebcf964a5201b6221e3"), OID("49b15db9f964a520d5521fe3"),
  OID("49e4b762f964a5201e631fe3"), OID("4baea9fff964a52067cc3be3"), OID("4e7ff9e4b80383b0e008c618"),
  OID("4d112ed05f3376eb368a1386"), OID("4d07873b9d33a143e367cc78"), OID("4abf8dd7f964a520139120e3"),
  OID("4ada3a78f964a520422021e3"), OID("4b6cb20cf964a520ea4c2ce3"), OID("4b9222d1f964a520dae833e3"),
  OID("4bf944e2bb5176b0328c5bb2"), OID("4c488c0131e41b8dd4745035"), OID("4c0123e819d8c928a5508829"),
  OID("4b439f6af964a5208fe425e3"), OID("4bd5021e6f64952164916eec"), OID("4b094ad8f964a520501523e3"),
  OID("4e2c957052b1b8f1986da3ef"), OID("4b3bb4b3f964a520467925e3"), OID("4e39205eaeb72d065964b8af"),
  OID("4b4258cff964a52016d225e3"), OID("4d70d062b48c236a417fd357"), OID("4dab13ec6a2303012f1b84ac"),
  OID("4b60bb01f964a52047f629e3"), OID("4d7ba7dd7498a1cd892f6efc"), OID("4bb78e4eb35776b072c4c701"),
  OID("4bf5d4245e800f47ba57e6d4"), OID("4b92cd1cf964a520171d34e3"), OID("4d835fa799b78cfab3d59e1f"),
  OID("4e77be77fa769112d4fe6465"), OID("4ac3f607f964a520a79d20e3"), OID("4b16ad7ef964a520e4bb23e3"),
  OID("4d9b2ec403d8721e96a3e124"), OID("4b82e25ef964a520e8ea30e3"), OID("4dfcdb54b61c84188eee403d"),
  OID("4c9d1641d3c2b60cebd4bdbc"), OID("4cd44e0ea5b346885f2b8750"), OID("4ddace5c2271ac9047657a5f"),
  OID("4b870349f964a52067ab31e3"), OID("4b312e7df964a520fc0125e3"), OID("4e8644f19adf376009a61900"),
  OID("4c9356a5f600236ade70c432"), OID("4aedc915f964a520e2ce21e3"), OID("4b3bcdf1f964a520827b25e3"),
  OID("4d150f8a8312236a8e824fba"), OID("4dc17e4e1838710f437ed107"), OID("4bd8d3132e6f0f47eccc0808"),
  OID("4bd4c5226f649521a3196eec"), OID("4e58abea2271886714e89aad"), OID("4b9ab395f964a5206cce35e3"),
  OID("4b65c9adf964a520bfff2ae3"), OID("4e57ded418a82d0a334c1eba"), OID("4a663032f964a5202fc81fe3"),
  OID("4e826a4e77c85da056dd387b"), OID("4cd571fa122ba143e60b2ca1"), OID("4d658cdb6d943704cfaf78bf"),
  OID("4540820cf964a5203c3c1fe3"), OID("4cb13eaff2dbef3b36dd79e5"), OID("4a3ecf0cf964a52075a31fe3"),
  OID("4c55203afd2ea5937fdb312b"), OID("4ba3e7e1f964a520486c38e3"), OID("4c60fc9a1e5cd13a6a0ea3ed"),
  OID("4b00400cf964a520f93b22e3"), OID("4be5bd27910020a15369d314"), OID("4bb7c509b35776b0ae26c801"),
  OID("43c51016f964a520632d1fe3"), OID("4cb127fb39458cfa096909a0"), OID("4d4e7e034a38b60c0a7dea66"),
  OID("4c8523bdd8086dcbe4fa8e52"), OID("4c88104f10523704098fbdf1"), OID("4bdfd3eb0ee3a593857a35b0"),
  OID("4bff5fdfe584c9285e296e25"), OID("4b9c6605f964a520bb6536e3"), OID("4b58239df964a520f74b28e3"),
  OID("4bb709be46d4a5938e0dc7c0"), OID("4c00e476ad15a59342938e73"), OID("4b671d69f964a520a13b2be3"),
  OID("4bf08bf117880f47a3782937"), OID("4c9386d6533aa093669fb645"), OID("4b13f559f964a520f19a23e3"),
  OID("4b9e5440f964a52064da36e3"), OID("4d9b551a7709cbff0c7e3ab2"), OID("4e53ff5a1495a46b65be98db"),
  OID("481354f5f964a5203a4f1fe3"), OID("4c295c3be19720a1887af958"), OID("4d36f5e7d8caa1cd2b4d72d6"),
  OID("4c9846df671db60cd144b8f6"), OID("4a43bcb7f964a520bba61fe3"), OID("4c6d73dde6b7b1f7cdc6a98e"),
  OID("4dc30fe1ae608779d0ff7bda"), OID("4b6061cbf964a520dbe229e3"), OID("4c35ded5a0ced13a5a6d1a6e"),
  OID("4ad4c00ef964a520f2ed20e3"), OID("4e2c0cb4b61cd010837b440d"), OID("4be533b8d4f7c9b6ee272520"),
  OID("4be46c7c910020a1e0b1d114"), OID("4bf82e038d30d13a3d4b0018"), OID("4b87952cf964a52071c331e3"),
  OID("4ec16d1cbe7b04923d264c7c"), OID("4b05b4c1f964a52047e122e3"), OID("4c8b9b752e3337042167ce41"),
  OID("4b1a85c5f964a52097eb23e3"), OID("4b199d10f964a5207ee023e3"), OID("4bac6730f964a52095f33ae3"),
  OID("4b166e4ef964a520fdb823e3"), OID("4af2dc2af964a520d0e821e3"), OID("4d08bd38611ff04dd77b1cfb"),
  OID("4b6335bff964a520d16a2ae3"), OID("4ba55045f964a5208ff938e3"), OID("43a3c20df964a5203f2c1fe3"),
  OID("4b8d6ad3f964a5206afa32e3"), OID("446d8fa5f964a52067331fe3"), OID("4b61c1fff964a520e1202ae3"),
  OID("4c34f5823ffc95212e0a91f5"), OID("4b537a46f964a520f29e27e3"), OID("4d9e3cf714013704d7ad12b6"),
  OID("4bb4402e87aa95216b0badd1"), OID("4b53f7ccf964a520baaf27e3"), OID("4b7b07b0f964a5204a4d2fe3"),
  OID("4ae8be15f964a520e0b121e3"), OID("4cfab116c51fa1cdbd55e22b"), OID("4b4b1876f964a520389226e3"),
  OID("4b529ebdf964a520218427e3"), OID("4b94a7aff964a520c07f34e3"), OID("4ba23ad0f964a5205de537e3"),
  OID("4ba2e967f964a520162238e3"), OID("4412be02f964a520d6301fe3"), OID("4adcdb01f964a520ad5e21e3"),
  OID("4c1dc0d0b4e62d7f3c55dd93"), OID("4adcdaa0f964a520a04d21e3"), OID("4b0b549ff964a520773023e3"),
  OID("4addf02df964a5207d6621e3"), OID("4d733b64ff6ba35d59417a8a"), OID("4b2d0e1af964a52037cd24e3"),
  OID("4b99bfc5f964a520e58f35e3"), OID("4c8cf4db5e048cfa0334d1cd"), OID("4ada5ad8f964a520bf2121e3"),
  OID("49ca8f4df964a520b9581fe3"), OID("4b97ce38f964a520bb1635e3"), OID("4c572a546201e21e394a556e"),
  OID("4ba3e14ff964a520936938e3"), OID("4bd9d9395f34b713a8df3579"), OID("4d3f8de546775481fc0050f4"),
  OID("4ca8d8ef44a8224bdfc81b40"), OID("4d432f28f354236a8476796d"), OID("4d41462d00e8a35dca8802fb"),
  OID("4e7251701f6ecfe82c35d18d"), OID("4dfcf57dd164848a03f780f3"), OID("4e2ce14ad22d3f83c89e8f5f"),
  OID("4c5ecbba7f661b8d8af24e1c"), OID("4be497ce910020a15df1d114"), OID("4e30422f62e1fbac61137f70"),
  OID("4c09aeb4ffb8c9b6786e6a61"), OID("4c09fd76009a0f476ac2e8bf"), OID("4e5139ecc65bb313ba9ea493"),
  OID("4bb5b72e46d4a593a145c5c0"), OID("4e7f61d661af411b6306a7d5"), OID("4e604d9c2271573ad6ae7229"),
  OID("4b525f22f964a520ea7927e3"), OID("4bfb45fdd0382d7fa188c90a"), OID("4b2c9a88f964a52014c824e3"),
  OID("4e67b9cfae609d64bd03e867"), OID("4a0768b0f964a5205c731fe3"), OID("4b50f751f964a520f53a27e3"),
  OID("4e7660f4cc3fc1186ed4599f"), OID("4d46cac61ed56dcbba6fc954"), OID("4b28eef1f964a520499624e3"),
  OID("4e8a58d8be7b5e7d9602bcde"), OID("4e698e55a809290267a15df3"), OID("4c2e4501e307d13a65470fda"),
  OID("4e99d3d6be7bc875aa3ebb7a"), OID("4d30af6982fd5481bca5b5a8"), OID("4bf1d84a99d02d7f7ea3c948"),
  OID("4c336c206f1fef3b208aec3d"), OID("49c43671f964a520ae561fe3"), OID("4c617d2554ac0f47e10bb821"),
  OID("4cd494a1122ba1434d6a26a1"), OID("45f412e6f964a520f6431fe3"), OID("4b6158ebf964a520af102ae3"),
  OID("4c66b5e0b80abe9a96a8cde5"), OID("4bd5eca66f649521ccf66fec"), OID("4e246d9ad22d0a3f5a1571bf"),
  OID("4c5aef82ec2520a104b75212"), OID("4d8b1ecd6ce6a35df7156e42"), OID("4e636dfbe4cdf1e2bfb7221d"),
  OID("4cc952babcb1b1f7f8470e8a"), OID("4df75fa2aeb7da11e1a8a74e"), OID("4ce93b078ef78cfa37909f9b"),
  OID("4d0610b992288eec008c9ce0"), OID("4e7bafdbae60ed9a4aafaa2b"), OID("4b4f0af6f964a52023f926e3"),
  OID("4bad56adf964a520b5473be3"), OID("4dfa656c18a8ab1870be07a0"), OID("4e814d8d6da173f429b24d1c"),
  OID("4bdb1dde63c5c9b6256b2668"), OID("4c9349bc911b8cfa2043f4b5"), OID("4bd054c677b29c74e07e8a82"),
  OID("4c67b3a67abde21e59426768"), OID("4bdadc2063c5c9b6d1362568"), OID("4b8bc3aff964a5203caa32e3"),
  OID("4ca35d3b7f84224b9e28c458"), OID("4bbe7a3782a2ef3b017d2bd2"), OID("4c2d6bc97d85a59399b252f3"),
  OID("4b652435f964a52009e62ae3"), OID("4e899083be7b57d7d6e9eec2"), OID("4c7428dadb52b1f7574875dc"),
  OID("4b677150f964a520394f2be3"), OID("4a36f332f964a5200c9e1fe3"), OID("4c38e2530a71c9b6508941c9"),
  OID("4b6741a3f964a52034442be3"), OID("4e97aebe6c25f5286029d5cd"), OID("4c5ada266407d13a9b99b528"),
  OID("4e02b3ebae6099b226e0a8ca"), OID("4df701ad483b96f73159134c"), OID("4e2abce352b1b8f1985935f1"),
  OID("4dca4a9a183817362fdf2570"), OID("4b6159ccf964a520c7102ae3"), OID("4d678fb12433a143d40c4de0"),
  OID("4a7dc82ef964a52098ef1fe3"), OID("4d0943ab9232236a6440c253"), OID("4c2b548b77cfe21e5247b4f1"),
  OID("4c1e915ab306c928be4467b7"), OID("4b621f1df964a520d9362ae3"), OID("4b7b6d7af964a5201d632fe3"),
  OID("4b565ebef964a5208c0d28e3"), OID("43c922c9f964a520972d1fe3"), OID("4dd557fe8877e2b7c5eeaecb"),
  OID("4bd7225529eb9c74f94396e1"), OID("4bddd6cbe75c0f4765d0c503"), OID("4b269ac4f964a520367e24e3"),
  OID("4a5ff47ff964a52090c01fe3"), OID("4b996ef9f964a5202d7b35e3"), OID("4bef2436f2712d7f9e5ffbd8"),
  OID("4e5c0dc0b99329e22ae14682"), OID("413e4b80f964a520521c1fe3"), OID("4c3639101e06d13a96d9733e"),
  OID("4cc36fbc91413704abfabb55"), OID("4e90950adab46521c1238498"), OID("4bcdb288fb84c9b67b60223e"),
  OID("4bf343ed98ac0f4713c062a8"), OID("4ca2e52e5720b1f7d2302def"), OID("4dbd47e5a86e0e98a1f72d18"),
  OID("4cd148ff6449a093041ccfcf"), OID("4d520d4ce02754819f0cb1b6"), OID("4cb3fed8c5e6a1cdd925f0f6"),
  OID("4e689b8745dd6cc5bd0e2b0a"), OID("4cf3e2bb8333224b1d01188e"), OID("4b63406af964a520a36d2ae3"),
  OID("4e930f1d8231bf0d17a4c2b7"), OID("4d2368ec2eb1f04d980008c2"), OID("4d07363830a58cfa783fafe7"),
  OID("4b2ca711f964a52053c824e3"), OID("4e7aef0b6365723c65113a69"), OID("4d38747c3ffba143fcac5c56"),
  OID("4b6dd554f964a52019942ce3"), OID("4e6617901838ad3d105c5f1f"), OID("4e298e4545ddfe8f9dfd264c"),
  OID("4ea2087be3008f7d91ea8814"), OID("4b3110bef964a52073ff24e3"), OID("4bcf762a0ffdce72dc81b2c0"),
  OID("4bb4564d49bdc9b64ab90c10"), OID("4b7f6ebff964a520552e30e3"), OID("4c79340abd346dcbd47ef4ef"),
  OID("4c4b5aaf5609c9b615570391"), OID("4cc44d71d43ba143970266f8"), OID("4b99a2e8f964a520f38835e3"),
  OID("4dfdf3cb7d8b30508019d0fa"), OID("4f4e9924e4b0f03ee60bb5dd"), OID("4f5b9ca0e4b0b28dd40cfa0c"),
  OID("4acbc300f964a52058c520e3"), OID("4cb18feb39458cfab5670ca0"), OID("4b772338f964a5201d812ee3"),
  OID("4c4535eb429a0f47a0e1491e"), OID("4b5d4c0cf964a5201f5929e3"), OID("4babbe56f964a52094c53ae3"),
  OID("4c967d8438dd8cfa8cf9db62"), OID("4c1091a43ce120a1b452081c"), OID("4aa44e08f964a5204f4620e3"),
  OID("4e8e4f94b8034831903baae9"), OID("4db588fd4df05e5aaaf43454"), OID("4bec61c2b68520a1ec081287"),
  OID("4b5b1038f964a52094e128e3"), OID("4bb38b8f42959c74fb16222c"), OID("4bf328ca354e9c74c88d2602"),
  OID("4e7a867ae4cdbba061cba356"), OID("446e0714f964a52075331fe3"), OID("4dea9aee7d8b6c7a53444996"),
  OID("4b5270f4f964a520737d27e3"), OID("4c6e8f7ed5c3a1cd5591c82b"), OID("4b5ce66ef964a5200c4a29e3"),
  OID("4e90179c2c5b7295bbb43f32"), OID("4bb41c28f187a593f1e913f8"), OID("4df8be7dae608a367cea8cbe"),
  OID("4b45f75ef964a520141326e3"), OID("4e6fbc74aeb7e676a77de125"), OID("4e89ed78f790d7f88f3a1e68"),
  OID("4bad1f39f964a520d52f3be3"), OID("4c6152ba54ac0f47519ab721"), OID("4b1168a6f964a520fb7b23e3"),
  OID("4e92d0baa17cdc523bbf56d0"), OID("4c1b4deec09ed13a1a74828e"), OID("4e907bdc6c25abdcf4b6ba68"),
  OID("47fd0d51f964a520da4e1fe3"), OID("4d9ab4a1674ca143e5a6c143"), OID("4b77064ff964a52088752ee3"),
  OID("4b228c46f964a520cc4824e3"), OID("4d42404eb0c354812143def1"), OID("4c68980a773720a1831480e9"),
  OID("4c51f13ab6dabe9a919e8b12"), OID("4c6009e523e303bb50d07407"), OID("4e427f7a18a8627fce4674c9"),
  OID("4e987b3502d5a39437282663"), OID("4e3cbc41fa76455375af719e"), OID("4b3a1855f964a520026125e3"),
  OID("4c8c4d0fc37a6dcba269f67a"), OID("4d100f5a71e8a1cd62977ebd"), OID("4e642825c65b2dc8a05fdbe3"),
  OID("4dd72350d4c05d5096e31bb2"), OID("4d35ce8d98336dcb893545f0"), OID("4b5236eef964a5204d7027e3"),
  OID("4c5de5f4bfa59521465585ff"), OID("4b60f5fff964a520a5032ae3"), OID("47a89b73f964a520914d1fe3"),
  OID("4e6095e2c65b2dc89ecd60d5"), OID("4ca8732b14c33704d1f3d33b"), OID("4be8e22588ed2d7f7872cc1d"),
  OID("4b43b39cf964a520e8e625e3"), OID("4e9876ece5e883cc4ba95805"), OID("4b3f972af964a520c8a925e3"),
  OID("4e552cec62e1d1844327a2fd"), OID("4e017b1be4cd338f2d48579a"), OID("4ba814ebf964a5205fcb39e3"),
  OID("4bef3f6fb0b376b050d6dab3"), OID("4b898d2ef964a520144132e3"), OID("4bae423ef964a520b2993be3"),
  OID("4b09f4d9f964a5205a2023e3"), OID("4bd33436046076b037707571"), OID("4bc2010a74a9a5930e8ad2f6"),
  OID("4abebd26f964a520a38f20e3"), OID("4b64b89cf964a520ffca2ae3"), OID("4bb70ca02ea195216b83ac2f"),
  OID("4b196193f964a52051dc23e3"), OID("4b927c38f964a52048fd33e3"), OID("4c6041721e5cd13acc2ea1ed"),
  OID("4da5dd2fcda1c55f755f88c5"), OID("4bd8696635aad13a63cb90f3"), OID("4bf277e455c7c9b69b1a6204"),
  OID("4c0c37aa009a0f47395becbf"), OID("4ddc4eb8c65bfd579e10aef5"), OID("4be1379ad816c9283e85efd9"),
  OID("4e74c557814ddff25ecca723"), OID("4dba6ab8f7b144688ff8d5b5"), OID("4cfba0b4d7206ea865e33f69"),
  OID("4d0df255e0b98cfa3b08df93"), OID("4c122eddb7b9c9281543a737"), OID("4b0e92eaf964a5209c5823e3"),
  OID("4e1cb53f52b1c2b69f37aaae"), OID("4cc3710f3d7fa1cd53cca55f"), OID("4b4e67e0f964a5209aec26e3"),
  OID("4a7b3bc0f964a5206cea1fe3"), OID("4b451265f964a520740326e3"), OID("4c3819041a38ef3ba38e9221"),
  OID("4b7b8a84f964a520ca662fe3"), OID("4b05887cf964a520d7c822e3"), OID("4b5f1fe0f964a5207ca729e3"),
  OID("4700946bf964a520344b1fe3"), OID("4e8008d8cc218f26a88833ae"), OID("4d48a18148a06dcb2d9865a2"),
  OID("4c8a2ae7e51e6dcbbf4666de"), OID("4dd39aa21f6e5374d6dce67a"), OID("4b6c6655f964a520ed362ce3"),
  OID("4bad7c4ef964a5209a553be3"), OID("4bcf1118937ca5933f72af92"), OID("4c6f10f39c6d6dcbde78ce7a"),
  OID("4b7629b5f964a520f2402ee3"), OID("4c1ab5c4b9f876b0ae217946"), OID("4cac7fc644a8224b0ade3340"),
  OID("4c63365286b6be9a48168e34"), OID("4ba3b1daf964a5204c5538e3"), OID("4bf2b0efa32e20a19f7cd557"),
  OID("4ccf406b7b685481008ebaf8"), OID("4cff902c7c563704a507abf0"), OID("412d2800f964a520df0c1fe3"),
  OID("4ad73509f964a520fb0821e3"), OID("4b05872cf964a5203c8322e3"), OID("45d012a3f964a520a5421fe3"),
  OID("4ded068c22719aa5524639a7"), OID("4c7acc782d3ba143297d92d0"), OID("4af59d8ef964a5205bfa21e3"),
  OID("4bd60d547b1876b027e38b86"), OID("4b6f179df964a52060dc2ce3"), OID("4bb5fe462ea19521b8f9aa2f"),
  OID("4aaeb8e5f964a5200c6320e3"), OID("4c4c1ed0f7cc1b8d6454da40"), OID("4d39bed4979ea14317e288fc"),
  OID("4bcba9c53740b713f3036365"), OID("4bfd2d138992a59361aeabb0"), OID("4b7491bff964a520b2e42de3"),
  OID("4cf768cb55e137047a91d2b6"), OID("4adaaca9f964a520052421e3"), OID("4bb76f3bf562ef3bb3643197"),
  OID("4c0cf59eb1b676b037ccdf86"), OID("4b9d06f9f964a520968936e3"), OID("40b13b00f964a520c7f51ee3"),
  OID("4d91df05d7b1236aca9f3938"), OID("4b072d95f964a520ebf822e3"), OID("4d8f7f1f6174a093cf9bdfe3"),
  OID("4bb9f9b71261d13af40bea98"), OID("4e566cf8ae6040e96e026674"), OID("4cfa8cb3fabc2d43e48edbd2"),
  OID("4adcda33f964a5206e3a21e3"), OID("4af48625f964a5207af321e3"), OID("4b490347f964a520f96126e3"),
  OID("4dc88e867d8b549a55f227e7"), OID("4d4d4c1cb887a1cde929baa0"), OID("4b98551ff964a5205b3c35e3"),
  OID("4bb4d42a6ebfc9b64173edfa"), OID("4d3c633a6e0aa1cd8f80e22c"), OID("4e539655483b944199df4b51"),
  OID("4d08761e1657a35d0ad62ce7"), OID("4d11e3f3b69f224b3dd17b55"), OID("4bffc9cef61ea59387bdea13"),
  OID("4cd23f3a9fcab1f797cd4993"), OID("4b73dc25f964a520fcbd2de3"), OID("4c8b5ad252a98cfafb2b35e9"),
  OID("4c871d44b231b60cad2017ec"), OID("4b5f96dbf964a52047c329e3"), OID("4c7f05f4fb74236aeb1ef9b9"),
  OID("4b97dc93f964a520d91a35e3"), OID("4b599377f964a5203e8d28e3"), OID("4c7845bddf08a1cd8be6d65d"),
  OID("4b2d109ef964a52076cd24e3"), OID("4b271087f964a5208b8424e3"), OID("4e492d7d62845e1d3cb4739a"),
  OID("4b29255af964a520b19924e3"), OID("4b5f1fcef964a52073a729e3"), OID("4ca5542576d3a093d65df76a"),
  OID("4de24d8d8877bcb686407001"), OID("4bf6936bbfeac928cc8b9436"), OID("4b3b9f37f964a5203c7725e3"),
  OID("49e45600f964a520fd621fe3"), OID("4a4a954af964a520f8ab1fe3"), OID("4bf7fa288d30d13a10d2ff17"),
  OID("4b81dd80f964a520c5c130e3"), OID("4d8809fabc848cfaa649a52b"), OID("4b6c1d6af964a52096242ce3"),
  OID("4b9dff2bf964a520d0c536e3"), OID("4c5d9a5485a1e21e80d85911"), OID("4d5fdf71291059418d2eee46"),
  OID("4b64c88af964a520d4cf2ae3"), OID("4ad4c01ff964a520f2f120e3"), OID("4d1189161483b1f71a9a8f3e"),
  OID("4cc442c891413704af82c155"), OID("4e0cdfec45dd7a490b0b32d4"), OID("4b8abb23f964a520187d32e3"),
  OID("4d7caaa1136bf04de321638d"), OID("4c51abed3940be9ac6d54009"), OID("4b638d7cf964a520e9822ae3"),
  OID("4e692313e4cda3907f1a5847"), OID("4af0c4e1f964a52002df21e3"), OID("4e2f6018aeb7e1b8afa523a9"),
  OID("4b91750ef964a520edbd33e3"), OID("4c4639f8f1b5c9b6b6f078ef"), OID("4bfc2b7aa118a5932b0f6eb6"),
  OID("4beab0e0415e20a1524ae5bb"), OID("4cb0a9e7aef16dcb15e5b254"), OID("4b5f8c55f964a520e3c029e3"),
  OID("4aec8733f964a5204cc821e3"), OID("4bfea164369476b09d938c1f"), OID("4cbef12d575d236aa3c5bb8e"),
  OID("4b59ef48f964a52015a228e3"), OID("4ab36f36f964a520a66d20e3"), OID("4bc0a8ccb492d13af195a460"),
  OID("4d16e1be401db60c0e60e8a4"), OID("4c4eaaf89efabe9a7748746a"), OID("4e90d507f5b9f8967cdcf673"),
  OID("4dfcbc72e4cdbe059c39f14f"), OID("4b3d1e76f964a5205e8e25e3"), OID("4b4f8c43f964a520f40a27e3"),
  OID("4bbce909593fef3b12cd0256"), OID("4ae7058af964a52000a821e3"), OID("4bec601e976ac928faea600b"),
  OID("4cccb411ee23a1437a0c21a8"), OID("4ba8d89af964a520b2f339e3"), OID("4b5ba2d0f964a520a90c29e3"),
  OID("4b9fa9aef964a520bd3237e3"), OID("45487bc1f964a520833c1fe3"), OID("4c4e0d33fa9d95217e63330a"),
  OID("4b707ce7f964a520901d2de3"), OID("4adbcc8ef964a520b52a21e3"), OID("4a5ac2f1f964a520d4ba1fe3"),
  OID("4c2f4631ed37a59303eb6603"), OID("4d036dd27d9ba35d3ba95f23"), OID("4e1c2f76e4cd1f7c5d7d8d83"),
  OID("4e560c6eaeb7a82687fdbb33"), OID("4d823c8e0d5b8cfa77da4f28"), OID("4be359622fc7d13a9987083a"),
  OID("4de9dbbcc65be8091d96b099"), OID("4ca8ea1ab0b8236af464b8e6"), OID("4bb37e2eeb3e95215032cb0a"),
  OID("45db8eb5f964a520fd421fe3"), OID("4b0492b2f964a520465522e3"), OID("4b39f2a2f964a520b85f25e3"),
  OID("4b8e6baef964a520db2133e3"), OID("4b5dd67bf964a520c76e29e3"), OID("4c51f970048b1b8d2457cf2f"),
  OID("4c2fae867cc0c9b64531ec9a"), OID("4bbeed39b083a59308aaa2e9"), OID("4ca4e077931bb60c9f0085e2"),
  OID("4c35367e3896e21e37f7ec90"), OID("4e175e97e4cd49a7e3df04df"), OID("4ca079b346978cfa3087b37f"),
  OID("4b705ceef964a52059152de3"), OID("4c2c6776b34ad13ad939ebce"), OID("4d872ed950913704e836ba5b"),
  OID("4bdb31a3f1499c74934838f3"), OID("4b12fa21f964a520f59123e3"), OID("4bbe0dcf8a4fb713f2c33d9d"),
  OID("4d3809a39784a093ec61dce8"), OID("4b80052af964a5203d4b30e3"), OID("4bbf67ca006dc9b6d788fc3f"),
  OID("4b0d0de8f964a520774323e3"), OID("4b6c7698f964a5202b3c2ce3"), OID("4e1e440dd16488cf82f5d114"),
  OID("4b64912cf964a5205abe2ae3"), OID("4ad6d40cf964a5204c0821e3"), OID("4dd7aee052b1a5c64451debd"),
  OID("4b636544f964a520c9762ae3"), OID("4ada7242f964a520a22221e3"), OID("4b4d84e8f964a52071d326e3"),
  OID("4d6f338927316ea8dd43adaa"), OID("4c5a549df54376b0a2ae848b"), OID("4ba7d4b6f964a52040b739e3"),
  OID("4c4f554a92b6a593184c6471"), OID("4e7d44ff2c5b9c05d6a7a836"), OID("4af757acf964a520920822e3"),
  OID("4e3239096284e26999452157"), OID("4b117396f964a520db7c23e3"), OID("4bf403496a31d13a2639952e"),
  OID("4b1b341bf964a5208af923e3"), OID("4bcafec4cc8cd13a1086becf"), OID("4c2f80ed213c2d7fc6f7305d"),
  OID("4e68bf0331513f0d3b3f653d"), OID("4d3102bcf8c9224b07bfa2d2"), OID("4ab3e778f964a5200c6f20e3"),
  OID("4e96c68ae5fa60039ed09345"), OID("4dc8e07ad164033c5733d103"), OID("4e02ad1152b10583352c56a4"),
  OID("4b57ac40f964a520073b28e3"), OID("4e5d210045dd2e4538b7b3c3"), OID("4c306321a0ced13af518126e"),
  OID("4d50f2a4747f6dcb2368bbd4"), OID("4b07fb62f964a520e00123e3"), OID("4a3407b4f964a520999b1fe3"),
  OID("4c1d4c30fcf8c9b6e691ab0b"), OID("4b91d635f964a520b9da33e3"), OID("4c90333a6fbf224b93fa518f"),
  OID("4d425b7ef0dba1cd77a33249"), OID("4ba8c979f964a520c7ee39e3"), OID("4a8b62f8f964a520540c20e3"),
  OID("4b9be006f964a520263036e3"), OID("4ae23e91f964a5204b8c21e3"), OID("4ae27553f964a5206e8e21e3"),
  OID("4b58f4dcf964a520587528e3"), OID("4bbc5eb5ed7776b03e883f51"), OID("4df8674aaeb70af3f2769b4f"),
  OID("4d072fa0c2e5370408d7c767"), OID("4b520b9ef964a520c16327e3"), OID("4b9687daf964a52081d234e3"),
  OID("4e701762a80980879ff65a22"), OID("4db2f8a4a86e8d2707652f4d"), OID("4be4aca68a8cb713ff4ec4a0"),
  OID("4be2dcab21d5a59364781711"), OID("4bdac5ca3904a5934c42479e"), OID("4bc4f5346c6f9c745dc7b3fc"),
  OID("4e905150b8039b4557d9e266"), OID("4dfb8f8aaeb7e41abbdffd22"), OID("4eb5515b61af0dda9049a19c"),
  OID("4d33d70cf8c9224b1120bed2"), OID("4cfeb23062a38cfa3d095a32"), OID("4e76244b62e1263516b93909"),
  OID("4e17d394a8097d08b23191c3"), OID("4bb7d11fb35776b0303bc801"), OID("4ad4bffdf964a52090ea20e3"),
  OID("4c0fe0e03ce120a16d2d071c"), OID("4b76477df964a52068462ee3"), OID("4b85eca2f964a520c27931e3"),
  OID("4bce045bc564ef3b3d80edf0"), OID("4bddfb4ee75c0f478468c603"), OID("4c13a24d7f7f2d7faf58df68"),
  OID("4c8ca0e7f0ce236a02f719ef"), OID("4bf3dd55e5eba593d7ef1e90"), OID("4e29be5b18a80bb0584562bf"),
  OID("4e1a284218a8166f739d92ae"), OID("4e1bf15714954075097bf336"), OID("4e23a6e852b1f82ffbb549bc"),
  OID("4dd9fe2ac65bee535aefe87f"), OID("4ea450aa9911214fd199202d"), OID("4b59db8ff964a5205f9c28e3"),
  OID("4c3d8a5fa97bbe9a1cf4fbdd"), OID("4740b92ff964a520734c1fe3"), OID("4cd768326e8b5941df1663d2"),
  OID("4bb216f5f964a520eab73ce3"), OID("4e218c0552b1f82ffb9f2f44"), OID("4e1d7cbe6284d5831b3aaa89"),
  OID("4bf50b50e5eba593c2482090"), OID("4b3bd8b4f964a520777c25e3"), OID("4db6f0614df05e5aab10d2ce"),
  OID("4e1c896efa76c49a16e37473"), OID("4cb728728db0a1433ea16f16"), OID("412d2800f964a520df0c1fe3"),
  OID("4d319c315017a0931ed3409b"), OID("4e3242581495d5790be46068"), OID("4b76fa1df964a52095702ee3"),
  OID("4b4438a4f964a520cff225e3"), OID("4cc18cb65684a35d1152b90d"), OID("4e552ac9aeb7b18559daac62"),
  OID("4d87c2ad26a36ea8d188bfad"), OID("4a23b274f964a520da7d1fe3"), OID("4b70b008f964a520b2292de3"),
  OID("4c87991356e037041e8eb3a3"), OID("4e35eba452b17fb1c60db51f"), OID("4b2d1295f964a520b0cd24e3"),
  OID("4e1b82e01f6e5708b2210b3b"), OID("4d31cb58c6cba35d9ba01a7a"), OID("4e57a73cd16491e0ae4bd2f1"),
  OID("4b2ee761f964a52012e824e3"), OID("4d08db88f6582d43bba23eaf"), OID("4b54d5ccf964a520e3ce27e3"),
  OID("4bd8c0a9e914a593a95f54fa"), OID("4bce3b8fef1095210dcc8386"), OID("4ce3488c22bef04d3af9d2f8"),
  OID("4a7dfc6af964a52082f01fe3"), OID("4baaeb2ff964a520d28d3ae3"), OID("46009f8af964a52099441fe3"),
  OID("4bd46b3b7b1876b012f08886"), OID("4b749309f964a52017e52de3"), OID("4bab9a52f964a5201db83ae3"),
  OID("49e59fabf964a520db631fe3"), OID("4e6bb984e4cdb375521f9701"), OID("4b7b68c4f964a52068622fe3"),
  OID("4e2ce6dfae605c533a4775aa"), OID("4b6b11aef964a520aaf02be3"), OID("4a78c47ff964a5205de61fe3"),
  OID("4b48d67df964a520595926e3"), OID("4d5e43a114963704b253c794"), OID("4ba2a5b3f964a520600c38e3"),
  OID("4e45d93052b1bac0d96f2963"), OID("4412d38af964a520d9301fe3"), OID("4a42b34ef964a52022a61fe3"),
  OID("4cdebfcaaba88cfaf5f443d7"), OID("4b9d14c7f964a520e68d36e3"), OID("4dab0e60fc607fc998bcc902"),
  OID("4dc967c6e4cd1818d5b757dd"), OID("4c0d9b0ab1b676b053e9e086"), OID("4caddf332303a143a7ede9f0"),
  OID("4c76d279947ca1cdf3734537"), OID("4de046abd22d2a79556af846"), OID("4c45f9a7342c1b8d383bb589"),
  OID("4bbbc1ab2d9ea593bbda9fce"), OID("4b646c29f964a520e7b12ae3"), OID("4da2332bb521224bf91105ee"),
  OID("4ba424caf964a520f98538e3"), OID("4d9ab9e2e086a35dcd0ac1ad"), OID("4e00b5b9d22db37fb025d475"),
  OID("4ba5565ff964a5204cfc38e3"), OID("4cc83ab5de08199ce208ed5e"), OID("4e9a34020aaf5f690e43edc5"),
  OID("4c8be1a75e048cfaf6a1c7cd"), OID("4daaf1196e81162ae7e14b69"), OID("4ac518f7f964a520a5af20e3"),
  OID("4bc83ebe6501c9b663be3f29"), OID("4ad4c00df964a5209eed20e3"), OID("4e361bbf1f6e9e21ad5608bb"),
  OID("4d0d7ea81f6bf04da0df7d31"), OID("4b3eb6cef964a52046a125e3"), OID("4c3176953896e21e3d22e790"),
  OID("4c486fd10f5aa593ff268176"), OID("4c5d2ee494fd0f47cbbfca45"), OID("4de9f370d164ef597cddd7a5"),
  OID("4da1dc29b3e7236adf531279"), OID("4e4ac632d164a7c8b6aaba5f"), OID("4e37c0cbae6076788f16baaf"),
  OID("4c236ea0fbe5c9b603669b21"), OID("4ca0a639cd439eb0eddeee2b"), OID("4b476f08f964a520ef3126e3"),
  OID("4b8a5d0ef964a520036a32e3"), OID("4bc63003bf29c9b6c167f92a"), OID("4b75fe3bf964a520fd342ee3"),
  OID("4e284d081f6e88a154608e0d"), OID("4c7280f3ad69b60ce99683b9"), OID("4ec146a5f7907411616f2aa3"),
  OID("4a90a5caf964a520f01820e3"), OID("4a876e1af964a520550420e3"), OID("4c78112cdf08a1cd9122d65d"),
  OID("4b60d733f964a520e3fc29e3"), OID("4bbb5890935e9521730d2990"), OID("4e0e2c8ee4cd27fc7d2d0bcd"),
  OID("4ddfc866185035f3a44eeae3"), OID("4aea4afaf964a520cbba21e3"), OID("4e3ad0f51838961aff046e72"),
  OID("4b9e3206f964a52000d136e3"), OID("4e52a162d22d41fba7a41ae4"), OID("4bd82c135cf276b093959c00"),
  OID("4d12356affa1224b0c8e97ad"), OID("4c8738a847cc224b3ba2af9f"), OID("4b058714f964a520867e22e3"),
  OID("4d969cd8daec224b9d321c3e"), OID("4b602d15f964a52096d829e3"), OID("4c79fb5aa8683704f598124d"),
  OID("4c118a9dd41e76b006a8310d"), OID("4b3b58d4f964a520c07225e3"), OID("4ad6207df964a5203d0521e3"),
  OID("4be4bffcbcef2d7fb20e03e5"), OID("4ae7615cf964a520cdaa21e3"), OID("4c5e90ae6ebe2d7f72fdd52e"),
  OID("4e7e4d1b0aafd656ed5762bf"), OID("4d8e32953bfef04d50c29286"), OID("4d98601f97d06ea83a2c320b"),
  OID("4e18de39d4c062b044ecfc46"), OID("4dfb4794b0fb73bb4d7933b1"), OID("4e82d8aa722e93408a889317"),
  OID("4dbaac255da389d2c24070eb"), OID("4baa6efaf964a520a9693ae3"), OID("4b687265f964a52029792be3"),
  OID("4de897f9814d89ee7e7cd56c"), OID("4b5b708af964a52043fe28e3"), OID("4c65c7878e9120a1f435d664"),
  OID("4b6de38af964a52003992ce3"), OID("4d6def142c542d432bbb75a6"), OID("4e460a74227128e1e8c68136"),
  OID("4b6c662ff964a520dc362ce3"), OID("4bae57f4f964a52061a53be3"), OID("4c238f4913c00f475b3c89de"),
  OID("4b64912cf964a5205abe2ae3"), OID("4d5efa7ab6b9a1cda4486951"), OID("4c856271d8086dcb84979152"),
  OID("4b4f537ef964a5206d0127e3"), OID("4b64a859f964a52026c62ae3"), OID("4b095912f964a520051623e3"),
  OID("4c793912bd346dcbfc92f4ef"), OID("4b47b9a9f964a5200b3c26e3"), OID("4bef3af4c80dc92837b227e3"),
  OID("4b343032f964a520ea2525e3"), OID("4b2317f9f964a520335324e3"), OID("4c6d755090c6ef3b1bd20736"),
  OID("4e8a9eb749018e0360cf19e4"), OID("4b8ef850f964a520eb4133e3"), OID("42a63500f964a52001251fe3"),
  OID("4e5273ede4cdb70cc52d69a0"), OID("4b1a8827f964a520beeb23e3"), OID("4cd4cf612944b1f7208d59ec"),
  OID("4c5043f41886c9b670c26728"), OID("4ad63ba0f964a520e70521e3"), OID("4d14f72c6d103704203e26bd"),
  OID("4bc7da668b7c9c74225837cf"), OID("4cc0815b7dc9a093f5153ff5"), OID("4ac236c5f964a520389820e3"),
  OID("4df632eb18a88611c6c5b2fe"), OID("4c1c0ce2eac020a129ef45c2"), OID("4cbea9f00adda35dea2918e3"),
  OID("4b757300f964a5201c0d2ee3"), OID("4db0823aa86e63d211598769"), OID("4b24389bf964a5200c6424e3"),
  OID("4eb94605991165b763fc7b43"), OID("4dfb479d45dd0d640c6718af"), OID("4e732ef0aeb74f9f4920b2cf"),
  OID("4c73cee4376da09370eba9c6"), OID("4aa80a86f964a520eb4e20e3"), OID("4bfadb0fab180f47f40cb3ce"),
  OID("4c11894c17002d7f7272e609"), OID("4aea6fd6f964a52093bb21e3"), OID("4ba53502f964a520e5eb38e3"),
  OID("4c7eaaf7d860b60c8c9e5e9d"), OID("4d7ad48e4755f04d516bf124"), OID("4b656c4ef964a52022f02ae3"),
  OID("4d6e4c1324b56ea8c305fefb"), OID("4ccc4b98ee23a1430ece1da8"), OID("4c7be77adf08a1cd55dae15d"),
  OID("4be33939660ec928367bcb3b"), OID("4e81ca100aaf2ecba998d914"), OID("4b6cb1d4f964a520d84c2ce3"),
  OID("4abd65aaf964a520108a20e3"), OID("4e0fb271fa76d62f445551ad"), OID("4ab66d3bf964a5200b7720e3"),
  OID("4b7757e8f964a5209b932ee3"), OID("4c87ab14821e9eb007cd8d89"), OID("4b588bbdf964a5202a5d28e3"),
  OID("4b5afc00f964a5206edd28e3"), OID("4c593871d3aee21e33a46755"), OID("4c26aec0c11dc9b6b77b2924"),
  OID("4d9cf5cd7f9e4eb95eec9cfc"), OID("4bbda4638ec3d13a37431c28"), OID("4b43d528f964a5206deb25e3"),
  OID("4c7d9a6e3b22a1cd7d895f9e"), OID("4c0280f439d476b021b22ea7"), OID("4b61b5eff964a520461e2ae3"),
  OID("4e31c4302fb6ede816e37dfc"), OID("4a8452eef964a52045fc1fe3"), OID("4af48625f964a5207af321e3"),
  OID("4b16b554f964a5205dbc23e3"), OID("4c689ef9428a0f47798e001b"), OID("4e8a3c6fe5fa5654b55e66bd"),
  OID("4ddf5fcfc65bb5e319d0253c"), OID("4dfabe7caeb7e41abbdd898d"), OID("4cff1fd11ebe6dcb44c77f91"),
  OID("4b911994f964a520c5a333e3"), OID("4aa68066f964a520fe4920e3"), OID("4c5a4f7d67ac0f47bf8f064c"),
  OID("4e7968d7ae60f8128c9ee3a1"), OID("4bfe05cd8992a593fd07adb0"), OID("4e44501b2271bdbcf672b711"),
  OID("4b5f3573f964a52070ad29e3"), OID("4cbb6bf9adcd54817ade28a2"), OID("4bc5843eccbcef3bf49ce6d2"),
  OID("4da5da20cda1c55f755f3d36"), OID("4adcdb23f964a520ef6021e3"), OID("4e009255c65b896d116aa28b"),
  OID("4e361f34fa7656ba31788eda"), OID("4bdc454a63c5c9b67eff2a68"), OID("4b895feaf964a5201a3032e3"),
  OID("4d477261847e6dcbe49193c4"), OID("4c95e27858d4b60c2c573629"), OID("4d946954e923721e67ed6afe"),
  OID("4e04b5d845ddb464557a05db"), OID("4b83cbbef964a520e41031e3"), OID("4d03351b7d9ba35d32865e23"),
  OID("4bce34ccef1095213bbb8386"), OID("4e540cde1495a46b65bf6243"), OID("4deebbae887754a6af82da79"),
  OID("4a09b393f964a52048741fe3"), OID("4ddd406a52b177ff2e7f72bd"), OID("4d9dd3bec593a1cd34fb6819"),
  OID("4bfd9ae44cf820a1f5b6ecf4"), OID("4b7c3f78f964a52039872fe3"), OID("4bbadb793db7b7130fc2239a"),
  OID("4d1af9d3dd3637047d11601a"), OID("4bbf5a0185fbb713ec037267"), OID("4ba75f05f964a520c98e39e3")
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
            findOne(threadId,
                    "foursquare.users",
                    BSON("_id" << 19455489));
        }
    };

    struct LookupUserByIDs : SimpleTest {
        virtual void oneIteration(int threadId) {
            queryAndExhaustCursor(threadId,
                                  "foursquare.users",
                                  BSON("_id" << BSON("$in" << vector<int>(userids, userids + (sizeof(userids) / sizeof(int))))));
        }
    };

    struct LookupUserByIDsNoExhaust : SimpleTest {
        virtual void oneIteration(int threadId) {
            query(threadId,
                  "foursquare.users",
                  BSON("_id" << BSON("$in" << vector<int>(userids, userids + (sizeof(userids) / sizeof(int))))));
        }
    };

    struct LookupUVAByUVDoubleInQuery : SimpleTest {
        virtual void oneIteration(int threadId) {
            const Query& q = BSON("_id.u" << BSON("$in" << vector<int>(userids, userids + 200)) <<
                       "_id.v" << BSON("$in" << vector<OID>(venueids, venueids + 200)));
            //cout << "uva query: " << q.toString() << endl;
            query(threadId,
                  "foursquare.user_venue_aggregations2",
                   q
                 );
        }
    };
}

namespace{
    struct TheTestSuite : TestSuite{
        TheTestSuite(){
          add<FSTests::LookupUserByID>();
          add<FSTests::LookupUserByIDs>();
          add<FSTests::LookupUserByIDsNoExhaust>();
          //add<FSTests::LookupUVAByUVDoubleInQuery>();
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
