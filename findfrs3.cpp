#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <execution>
#include <functional>
#include <tuple>
#include <iterator>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <charconv>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/parallel_sort.h>

std::string cuttleFishPrefix;
int kappa = 1;
double alpha = 0.8;
int minSupport = 20;
int minAvgLen = 100;
int kSize;
std::ofstream outputFile;

unsigned int nThreads;

std::map<uint64_t, uint32_t> nodeIDToLen;

struct MyPairHasher {
    std::size_t operator()(const std::pair<uint64_t, uint64_t>& p) const {
        return std::hash<uint64_t>{}(p.first) ^ (std::hash<uint64_t>{}(p.second) << 1);
    }
};
tbb::concurrent_unordered_map<std::pair<uint64_t, uint64_t>, std::atomic<uint64_t>, MyPairHasher> pairSupport;

tbb::concurrent_hash_map<uint64_t, std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>> frSupport;

tbb::concurrent_hash_map<uint64_t, std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>> pathFRsup;

tbb::concurrent_hash_map<uint64_t, uint64_t> frTotalLen;

std::map<uint64_t, u_int32_t> frID;
std::map<uint64_t, std::vector<std::string>> frVar;

std::map<uint64_t, uint64_t> parentMap;
std::map<uint64_t, uint64_t> sizeMap;

int parsePositiveInt(const char* value, const char* name) {
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed < 1 || parsed > std::numeric_limits<int>::max()) {
        throw std::invalid_argument(std::string("Invalid ") + name + ": " + value);
    }
    return static_cast<int>(parsed);
}

double parseAlpha(const char* value) {
    char* end = nullptr;
    errno = 0;
    double parsed = std::strtod(value, &end);
    if (errno == ERANGE || end == value || *end != '\0' ||
        !std::isfinite(parsed) || parsed <= 0.0 || parsed > 1.0) {
        throw std::invalid_argument(std::string("Invalid alpha: ") + value);
    }
    return parsed;
}

class ThreadPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop = false;
    std::atomic<int> active_tasks{0};

public:
    ThreadPool(size_t threads) {
        if (threads == 0) {
            throw std::invalid_argument("Thread pool requires at least one thread");
        }
        for(size_t i = 0; i < threads; ++i)
            workers.emplace_back([this] {
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    active_tasks++;
                    task();
                    active_tasks--;
                    condition.notify_all();
                }
            });
    }

    template<class F, class... Args>
    void enqueue(F&& f, Args&&... args) {
        auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace([task]() { task(); });
        }
        condition.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(queue_mutex);
        condition.wait(lock, [this] {
            return tasks.empty() && active_tasks.load() == 0;
        });
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            condition.wait(lock, [this] {
                return tasks.empty() && active_tasks.load() == 0;
            });
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers) {
            if(worker.joinable()) worker.join();
        }
    }
};


uint64_t find(uint64_t i) {
    if (parentMap[i] != i) {
        parentMap[i] = find(parentMap[i]);
    }
    return parentMap[i];
}

void unionSets(uint64_t i, uint64_t j){
    if (find(i) != find(j)){
        sizeMap[find(i)] += sizeMap[find(j)];
        parentMap[find(j)] = find(i);
    }
}

void readSeg() {
    std::ios::sync_with_stdio(false);
    std::ifstream inputFile(cuttleFishPrefix + ".cf_seg", std::ios::in | std::ios::binary);
    std::vector<char> buf(1 << 26); // 64 MiB input buffer
    inputFile.rdbuf()->pubsetbuf(buf.data(), buf.size());
    if (!inputFile.is_open()) {
        throw std::runtime_error("Unable to open input file: " + cuttleFishPrefix + ".cf_seg");
    }
    std::string line;
    uint64_t numNodes = 0;
    kSize = INT32_MAX;
    while (std::getline(inputFile, line)) { 
        std::stringstream ss(line);
        uint64_t node;
        std::string label;
        ss >> node >> label;
        if (!ss || node == 0 || label.empty()) {
            throw std::runtime_error("Malformed line in " + cuttleFishPrefix +
                                     ".cf_seg: " + line);
        }
        nodeIDToLen[node] = label.size();
        parentMap[node] = node;
        sizeMap[node] = 1;
        kSize = std::min(kSize, static_cast<int>(label.size()));
        numNodes++;
    }
    if (inputFile.bad()) {
        throw std::runtime_error("Error while reading " + cuttleFishPrefix + ".cf_seg");
    }
    if (numNodes == 0) {
        throw std::runtime_error("Input file contains no nodes: " + cuttleFishPrefix + ".cf_seg");
    }
    std::cout << "Number of nodes read: " << numNodes << ", k determined: " << kSize << std::endl;
    inputFile.close(); 
}

struct Data {
    std::string name;
    std::vector<uint64_t> path;
};

std::istream& operator>>(std::istream& is, Data& r) {
    if (!(is >> r.name)) return is;
    r.path.clear();
    // Each path is a name followed by node IDs on the same line.
    while (true) {
        int ch;
        while ((ch = is.peek()) == ' ' || ch == '\t' || ch == '\r') {
            is.get(); 
        }
        if (ch == '\n' || ch == EOF) {
            if (ch == '\n') is.get();
            break;
        }
        uint64_t val;
        char sign;
        if (is >> val >> sign) {
            r.path.push_back(val);
        } else {
            is.clear();
            break;
        }
    }
    return is;
}

void readSeq(std::function <void(std::string, std::uint64_t, std::vector<uint64_t>)> processPath, bool parallel = true) {
    ThreadPool pool(std::max<size_t>(1, nThreads - 1));
    std::ios::sync_with_stdio(false);
    std::ifstream inputFile(cuttleFishPrefix + ".cf_seq", std::ios::in | std::ios::binary);
    std::vector<char> buf(1 << 26); // 64 MiB input buffer
    inputFile.rdbuf()->pubsetbuf(buf.data(), buf.size());
    if (!inputFile.is_open()) {
        throw std::runtime_error("Unable to open input file: " + cuttleFishPrefix + ".cf_seq");
    }
    uint64_t pathNum = 0, nodeCount = 0, prevNodeCount = 0;
    std::cout << "reading paths (. per 10^9 nodes): "; 
    std::cout.flush();

    std::istream_iterator<Data> dataRead(inputFile);
    std::istream_iterator<Data> EOFDataRead;
    while (dataRead != EOFDataRead) {
        for (uint64_t node : dataRead->path) {
            if (nodeIDToLen.find(node) == nodeIDToLen.end()) {
                throw std::runtime_error("Unknown node " + std::to_string(node) +
                                         " in path " + dataRead->name);
            }
        }
        if (parallel) {
            pool.enqueue(processPath, dataRead->name, pathNum, std::move(dataRead->path));
        } else {
            processPath(dataRead->name, pathNum, dataRead->path);
        }

        pathNum++;
        prevNodeCount = nodeCount;
        nodeCount += dataRead->path.size();
        if (nodeCount/1000000000 - prevNodeCount/1000000000 >= 1) {
            for (uint64_t i = 0; i < nodeCount/1000000000 - prevNodeCount/1000000000; ++i) {
                std::cout << "."; 
            }
            std::cout.flush();
        }
        ++dataRead;
    }
    if (parallel) {
        pool.wait();
    }
    if (inputFile.bad()) {
        throw std::runtime_error("Error while reading " + cuttleFishPrefix + ".cf_seq");
    }
    if (pathNum == 0) {
        throw std::runtime_error("Input file contains no paths: " + cuttleFishPrefix + ".cf_seq");
    }
    std::cout << "done" << std::endl;
    inputFile.close(); 
}

void processPathPairs(std::string pathName, uint64_t pathNum, std::vector<uint64_t> path) {
    // Track recently seen node clusters and pairs within each path.
    std::map<uint64_t, uint64_t> lMap;
    std::map<uint64_t, uint64_t> lsMap;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> dMap;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> dsMap;
    for (size_t i = 0; i < path.size(); ++i) {
        uint64_t node = find(path[i]);
        if (lMap.find(node) == lMap.end()) {
            lMap[node] = 0;
            lsMap[node] = i;
        }
        for (auto it = lMap.begin(); it != lMap.end(); ++it) {
            if (it->first != node) {
                std::pair<uint64_t, uint64_t> pairKey = std::make_pair(std::min(it->first, node), std::max(it->first, node));
                if (dMap.find(pairKey) == dMap.end()) {
                    dMap[pairKey] = 0;
                    dsMap[pairKey] = lsMap[it->first];
                }
            }
        }
        for (auto it = lMap.begin(); it != lMap.end(); ) {
            if (it->first == node) {
                it->second = 0;
                ++it;
            } else {
                it->second++;
                if (it->second > kappa) {
                    lsMap.erase(it->first);
                    it = lMap.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        for (auto it = dMap.begin(); it != dMap.end(); ) {
            if (it->first.first == node || it->first.second == node) {
                it->second = 0;
            } else {
                it->second++; 
            }
            if (it->second > kappa || i == path.size() - 1) {
                int start = dsMap[it->first];
                int end = i - it->second;
                std::set<uint64_t> uniqueNodes;
                for (int j = start; j <= end; ++j) {
                    if (find(path[j]) == find(it->first.first) || find(path[j]) == find(it->first.second)) {
                        uniqueNodes.insert(path[j]);
                    }
                }
                if (uniqueNodes.size() >= alpha * (sizeMap[it->first.first] + sizeMap[it->first.second])) {
                    pairSupport[it->first]++;
                }
                dsMap.erase(it->first);
                it = dMap.erase(it);
            }
            else {
                ++it;
            }
        }
    }
}

void processPathFRs(std::string pathName, uint64_t pathNum,  std::vector<uint64_t> path) {
    std::map<uint64_t, uint64_t> lMap;
    std::map<uint64_t, uint64_t> lsMap;
    for (size_t i = 0; i < path.size(); ++i) {
        uint64_t node = find(path[i]);
        if (lMap.find(node) == lMap.end()) {
            lMap[node] = 0;
            lsMap[node] = i;
        }
        for (auto it = lMap.begin(); it != lMap.end(); ) {
            if (it->first == node) {
                it->second = 0;
            } else {
                it->second++;
            }
            if (it->second > kappa || i == path.size() - 1) {
                uint64_t start = lsMap[it->first];
                uint64_t end = i - it->second;
                std::set<uint64_t> uniqueNodes;
                for (uint64_t j = start; j <= end; ++j) {
                    if (find(path[j]) == find(it->first)) {
                        uniqueNodes.insert(path[j]);
                    }
                }
                if (uniqueNodes.size() >= alpha * sizeMap[it->first]) {
                    tbb::concurrent_hash_map<uint64_t, std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>>::accessor acc;
                    frSupport.insert(acc, it->first);
                    acc->second.push_back(std::make_tuple(pathNum, start, end));
                }
                lsMap.erase(it->first);
                it = lMap.erase(it);
            } else {
                ++it;
            }
        }
    }
}


void processPathFRLen(std::string pathName, uint64_t pathNum,  std::vector<uint64_t> path) {
    tbb::concurrent_hash_map<uint64_t, std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>>::const_accessor pathAcc;
    if (pathFRsup.find(pathAcc, pathNum)) {
        for (const auto& entry : pathAcc->second) {
            uint64_t fr = std::get<0>(entry);
            uint64_t start = std::get<1>(entry);
            uint64_t end = std::get<2>(entry);
            uint64_t length;
            for (uint64_t i = start; i <= end; ++i) {
                if (i == start) {
                    length = nodeIDToLen[path[i]]; 
                } else {
                    length += nodeIDToLen[path[i]] - (kSize - 1);
                }
            } 
            tbb::concurrent_hash_map<uint64_t, uint64_t>::accessor acc;
            frTotalLen.insert(acc, fr);
            acc->second += length;
        }
    }
}

void processPathFRVars(std::string pathName, uint64_t pathNum, std::vector<uint64_t> path) {
    // Convert path node indexes to sequence coordinates, accounting for k-mer overlap.
    std::vector<uint64_t> nodeStart;
    for (u_int64_t i = 0; i < path.size(); ++i) {
        if (i == 0){
            nodeStart.push_back(0);
        } else {
            nodeStart.push_back(nodeStart[i - 1] + nodeIDToLen[path[i - 1]] - (kSize - 1));
        }
    }
    tbb::concurrent_hash_map<uint64_t, std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>>::const_accessor pathAcc;
    if (pathFRsup.find(pathAcc, pathNum)) {
        for (const auto &entry : pathAcc->second) {
            uint64_t fr = std::get<0>(entry);
            uint64_t start = std::get<1>(entry);
            uint64_t end = std::get<2>(entry);
            if (frID.find(fr) == frID.end()) {
                continue;
            }
            std::string pathSegment = "";
            for (uint64_t i = start; i <= end; ++i) {
                pathSegment += "-" + std::to_string(path[i]);
            }
            if (std::find(frVar[fr].begin(), frVar[fr].end(), pathSegment) == frVar[fr].end()) {
                frVar[fr].push_back(pathSegment); // Insert the path segment into the vector of segments for the corresponding FR in frVar if it's not already present
            }
            uint64_t varIndex = std::find(frVar[fr].begin(), frVar[fr].end(), pathSegment) - frVar[fr].begin();
            uint64_t seqStart = nodeStart[start];
            uint64_t seqEnd = nodeStart[end] + nodeIDToLen[path[end]];
            outputFile << pathName << "\t" << seqStart << "\t" << seqEnd << "\t"
                       << frID[fr] << ":" << varIndex << std::endl;
        }
    }
}

void clusterNodes() {
    uint64_t mergedCount;
    bool foundMerge = true;
    u_int64_t lastSize = 0;
    std::map<uint64_t, bool> marked;
    while (foundMerge) {
        pairSupport.clear();
        marked.clear();
        std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint64_t>> pairSupVec;
        pairSupVec.clear();
        if (lastSize > 0) {
            pairSupVec.reserve(lastSize);
        }
        mergedCount = 0;
        foundMerge = false;
        readSeq(processPathPairs);
        for (const auto& entry : pairSupport) {
            if (entry.second >= minSupport) {
                pairSupVec.push_back(entry);
            }
        }
        lastSize = pairSupVec.size();
        tbb::parallel_sort(pairSupVec.begin(), pairSupVec.end(), [](const auto& a, const auto& b) {
            return a.second > b.second; 
        });
        for (const auto& entry : pairSupVec) {
            if (! marked[entry.first.first] && ! marked[entry.first.second]) {
                unionSets(entry.first.first, entry.first.second);
                marked[entry.first.first] = true;
                marked[entry.first.second] = true;
                foundMerge = true;
                mergedCount++;
            }
        }
        std::cout << "Merged " << mergedCount << " pairs in this iteration." << std::endl;
    }
    pairSupport.clear(); 
}

void findFRs() {
    std::cout << "finding supporting paths" << std::endl;
    readSeq(processPathFRs);
    std::vector<std::pair<uint64_t, uint64_t>> frSupVec;
    for (const auto& entry : frSupport) {
        if (entry.second.size() >= minSupport) {
            frSupVec.push_back(std::make_pair(entry.first, entry.second.size()));
        }
    }
    tbb::parallel_sort(frSupVec.begin(), frSupVec.end(), [](const auto& a, const auto& b) {
        return a.second > b.second; 
    });
    for (const auto& entry : frSupVec) {
        tbb::concurrent_hash_map<uint64_t, std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>>::const_accessor acc;
        if (frSupport.find(acc, entry.first)) {
            for (const auto& supPath : acc->second) {
                uint64_t pathKey = std::get<0>(supPath);
                tbb::concurrent_hash_map<uint64_t, std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>>::accessor pathAcc;
                pathFRsup.insert(pathAcc, pathKey);
                pathAcc->second.push_back(std::make_tuple(entry.first, std::get<1>(supPath), std::get<2>(supPath)));
            }
        }
    }
    uint64_t frNum = 0;
    readSeq(processPathFRLen);
    for (const auto& entry : frSupVec) {
        tbb::concurrent_hash_map<uint64_t, uint64_t>::const_accessor acc;
        uint64_t totalLen = 0;
        if (frTotalLen.find(acc, entry.first)) {
            totalLen = acc->second;
        }
        if (totalLen < minAvgLen * entry.second) {
            continue;
        }
        frID[entry.first] = frNum++;
    }
    std::ostringstream oss;
    oss << alpha;
    std::string as = oss.str();
    std::string outputFileName = cuttleFishPrefix + ".ka" + std::to_string(kappa) + ".a" + as + ".ms" + std::to_string(minSupport) + ".ml" + std::to_string(minAvgLen) + ".bed";
    outputFile.open(outputFileName);
    if (!outputFile.is_open()) {
        throw std::runtime_error("Unable to open output file: " + outputFileName);
    }
    readSeq(processPathFRVars, false);
    if (!outputFile) {
        throw std::runtime_error("Error while writing output file: " + outputFileName);
    }
    outputFile.close();
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 6) {
            std::cerr << "Error: Incorrect number of arguments.\n";
            std::cerr << "Expected 5 arguments: <kappa> <alpha> <minSup> <minAvgLen> <cuttleFishPrefix>\n";
            std::cerr << "Example usage: findfrs3 1 0.75 20 200 /path/to/cuttlefish/prefix\n";
            return 1;
        }
        kappa = parsePositiveInt(argv[1], "kappa");
        alpha = parseAlpha(argv[2]);
        minSupport = parsePositiveInt(argv[3], "minimum support");
        minAvgLen = parsePositiveInt(argv[4], "minimum average length");
        cuttleFishPrefix = argv[5];
        if (cuttleFishPrefix.empty()) {
            throw std::invalid_argument("CuttleFish prefix cannot be empty");
        }
        std::cout << "Parameters: kappa = " << kappa << ", alpha = " << alpha
                  << ", minSupport = " << minSupport
                  << ", minAvgLen = " << minAvgLen << std::endl;
        std::cout << "CuttleFish prefix: " << cuttleFishPrefix << std::endl;

        nThreads = std::thread::hardware_concurrency();
        if (nThreads == 0) {
            std::cout << "The value is not computable, defaulting to 2.\n";
            nThreads = 2;
        } else {
            std::cout << nThreads << " concurrent threads are supported.\n";
        }

        readSeg();
        clusterNodes();
        findFRs();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        return 1;
    }

    return 0;
}