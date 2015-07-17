/**
 * @file    log_sort.cpp
 * @ingroup group
 * @author  Tony Pan<tpan7@gatech.edu>
 * @brief   Implements the de bruijn graph clustering in log(D_max) time.
 *          This file is only used for benchmarking partitioning algorithm in SC15. 
 *          For application use, please see src/getHistogram.cpp
 *
 * Copyright (c) 2015 Georgia Institute of Technology. All Rights Reserved.
 */
//Includes
#include <cstdlib>  // atoi
#include <mpi.h>
#include <iostream>

//File includes from BLISS
#include <common/kmer.hpp>
#include <common/base_types.hpp>

//Own includes
#include "sortTuples.hpp"
#include "parallel_fastq_iterate.hpp"
#include "configParam.hpp"
#include "argvparser.h"

#include <mxx/collective.hpp>
#include <mxx/distribution.hpp>
#include <mxx/reduction.hpp>
#include <mxx/timer.hpp>

#include <sstream>

using namespace std;
using namespace CommandLineProcessing;

#include <generator/make_graph.h>
#include <tuple>
#include <vector>

class Graph500Generator {
public:
  typedef int64_t id_type;
  typedef ::std::tuple<id_type, id_type, id_type>  tuple_type;

  static void generate(cmdLineParamsGraph500 &cmdLineVals, std::vector< tuple_type > &localVector) {

    size_t scale = cmdLineVals.scale;
    size_t edgefactor = cmdLineVals.edgefactor;

    // nedges and result are both local
    int64_t nedges;
    id_type* result;

    double initiator[] = {.57, .19, .19, .05};

    make_graph(scale, edgefactor * (1UL << scale), 1, 2, initiator, &nedges, &result );

    int p;
    MPI_Comm_size(MPI_COMM_WORLD, &p);

    // copy into the local vector
    localVector.reserve(nedges);
    id_type id;
    for (int i = 0; i < nedges; ++i) {
      id = result[2*i];
      if (id != -1) {  // valid edge
        localVector.emplace_back(id, id, result[2*i+1]);
      } // -1 need to be ignored.
    }

    free(result);
  }

};

template <typename T>
std::vector<T> get_partition_seeds(std::vector<T> &vector, MPI_Comm comm) {
  // use last seed as splitter
  int rank;
  int p;
  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &p);

  //Lets ensure Pn and Pc are equal for every tuple
  //This was not ensured during the program run
  std::for_each(vector.begin(), vector.end(), [](T &t){ std::get<kmerTuple::Pn>(t) = std::get<kmerTuple::Pc>(t);});

  // block partition
  if (p > 1) {
    mxx::block_decompose(vector, comm);

    // mxx_sort
    mxx::sort(vector.begin(), vector.end(), [](const T& x, const T&y){
      return std::get<kmerTuple::Pc>(x) < std::get<kmerTuple::Pc>(y);
    }, comm, false);
  } else {
    std::sort(vector.begin(), vector.end(), [](const T& x, const T&y){
          return std::get<kmerTuple::Pc>(x) < std::get<kmerTuple::Pc>(y);
    });
  }
  //printf("rank %d vert count : %lu first %ld last %ld\n", rank, vector.size(), std::get<kmerTuple::Pc>(vector.front()), std::get<kmerTuple::Pc>(vector.back()));


  // local unique - since we need to rebalance and do another round of unique, copy the unique to a new vector.
  std::vector<T> seeds;
  if (vector.size() > 0) {
    T prev = vector.front();
    seeds.emplace_back(prev);  // copy in the first one - this is definitely unique

    for (int i = 1; i < vector.size(); ++i) {
      if (std::get<kmerTuple::Pc>(prev) < std::get<kmerTuple::Pc>(vector[i])) {
        prev = vector[i];
        seeds.emplace_back(prev);
      }
    }
  }
  //printf("rank %d vert count : %lu seeds count: %lu\n", rank, vector.size(), seeds.size());


  if (p > 1) {

    std::vector<T> splitters;
    if ((seeds.size() > 0) && (rank > 0)) {
      splitters.emplace_back(seeds.front());
    }
    mxx::allgatherv(splitters, comm).swap(splitters);

    //== compute the send count
    std::vector<int> send_counts(p, 0);
    if (seeds.size() > 0) {
      //== since both sorted, search map entry in buffer - mlog(n).  the otherway around is nlog(m).
      // note that map contains splitters.  range defined as [map[i], map[i+1]), so when searching in buffer using entry in map, search via lower_bound
      auto b = seeds.begin();
      auto e = b;
      for (int i = 0; i < splitters.size(); ++i) {
        e = ::std::lower_bound(b, seeds.end(), splitters[i], [](const T& x, const T&y) {
          return std::get<kmerTuple::Pc>(x) == std::get<kmerTuple::Pc>(y);
        });
        send_counts[i] = ::std::distance(b, e);
        b = e;
      }
      // last 1
      send_counts[splitters.size()] = ::std::distance(b, seeds.end());
    }

//    for (int i = 0; i < p; ++i) {
//      printf("rank %d send to %d %d\n", rank, i, send_counts[i]);
//    }

    // a2a to redistribute seeds so we can do unique one more time.
    mxx::all2all(seeds, send_counts, comm).swap(seeds);

    // local sort and local unique
    std::sort(seeds.begin(), seeds.end(), [](const T& x, const T&y){
      return std::get<kmerTuple::Pc>(x) < std::get<kmerTuple::Pc>(y);
    });

    auto newend = std::unique(seeds.begin(), seeds.end(), [](const T& x, const T&y){
      return std::get<kmerTuple::Pc>(x) == std::get<kmerTuple::Pc>(y);
    });
    seeds.erase(newend, seeds.end());

    //printf("rank %d vert count : %lu seeds final count: %lu\n", rank, vector.size(), seeds.size());

  }

  return seeds;

}


// parallel/MPI log(D_max) implementation
void cluster_reads_par(cmdLineParamsGraph500 &cmdLineVals)
{
  static constexpr int Pn = kmerTuple::Pn;
  static constexpr int Pc = kmerTuple::Pc;


  //Define tuple type
  typedef typename Graph500Generator::tuple_type tuple_t;

  // get communicaiton parameters
  int rank, p;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &p);

  if(!rank) {
    std::cout << "Runnning with " << p << " processors.\n"; 
    std::cout << "Scale : " <<  cmdLineVals.scale << "\n";
    std::cout << "EdgeFactor : " <<  cmdLineVals.edgefactor << "\n";
  }

  mxx::timer t;
  double startTime = t.elapsed();

  /*
   * Indices inside tuple will go like this:
   * 0 : KmerId
   * 1 : P_new
   * 2 : P_old
   */

  MP_TIMER_START();

  // Populate localVector for each rank and return the vector with all the tuples
  std::vector<tuple_t> localVector;

  //Read all the kmers without any filter
  Graph500Generator::generate(cmdLineVals, localVector);

  MP_TIMER_END_SECTION("Generating Data");

  assert(localVector.size() > 0);
  auto start = localVector.begin();
  auto end = localVector.end();
  auto pend = end;


  //Sort tuples by KmerId
  bool keepGoing = true;
  int countIterations = 0;
  // sort by k-mer is first step (and then never again done)

  // sort by k-mers and update Pn
  mxx::sort(start, pend, layer_comparator<kmerTuple::kmer, tuple_t>(), MPI_COMM_WORLD, false);
  typedef KmerReduceAndMarkAsInactive<tuple_t> KmerReducerType;
  KmerReducerType r1;
  r1(start, pend, MPI_COMM_WORLD);
  MP_TIMER_END_SECTION("iteration KMER phase completed");


  // remove the MAX thing after k-mer reduce:
  // TODO: own k-mer reduction function!
  for (auto it = localVector.begin(); it != localVector.end(); ++it)
  {
    if (std::get<Pn>(*it) >= std::numeric_limits<int>::max()-Pn)
      std::get<Pn>(*it) = std::get<Pc>(*it);
  }

  while (keepGoing) {
    mxx::sort(localVector.begin(), localVector.end(),
        [](const tuple_t& x, const tuple_t&y){
        return (std::get<Pc>(x) < std::get<Pc>(y)
            || (std::get<Pc>(x) == std::get<Pc>(y)
            && std::get<Pn>(x) < std::get<Pn>(y)));
        }, MPI_COMM_WORLD, false);
    MP_TIMER_END_SECTION("mxx::sort");

    layer_comparator<Pc, tuple_t> pc_comp;

    // scan and find new p
    auto begin = localVector.begin();
    auto end = localVector.end();
    std::vector<tuple_t> newtuples;
    bool done = true;

    std::size_t local_size = end - begin;
    std::vector<std::size_t> distr = mxx::allgather(local_size, MPI_COMM_WORLD);
    if (rank == 0) {
      //std::cout << "local_sizes: [MAX : " << *std::max_element(distr.begin(), distr.end()) << ", MIN: " << *std::min_element(distr.begin(), distr.end()) << ", SUM: " << std::accumulate(distr.begin(), distr.end(), 0) << "]" << std::endl;
      //std::cout << "local_sizes: " << distr << std::endl;
    }

    // find last bucket start and send across boundaries!
    tuple_t last_min = *(end-1);
    last_min = *std::lower_bound(begin, end, last_min, pc_comp);
    // for each processor, get the first element of the last most element
    tuple_t prev_min = mxx::exscan(last_min,
        [](const tuple_t& x, const tuple_t& y){
          // return max Pc, if equal, return min Pn
          if (std::get<Pc>(x) < std::get<Pc>(y) ||
            (std::get<Pc>(x) == std::get<Pc>(y)
             && std::get<Pn>(x) > std::get<Pn>(y)))
          return y;
          else return x;}
        , MPI_COMM_WORLD);
    tuple_t prev_el = mxx::right_shift(*(end-1), MPI_COMM_WORLD);

    // get the next element
    tuple_t first_max = *(std::upper_bound(begin, end, *begin, pc_comp)-1);
    tuple_t next_max = mxx::reverse_exscan(first_max,
        [](const tuple_t& x, const tuple_t& y){
          // return min Pc, if equal, return max Pn
          if (std::get<Pc>(x) > std::get<Pc>(y) ||
            (std::get<Pc>(x) == std::get<Pc>(y)
             && std::get<Pn>(x) < std::get<Pn>(y)))
          return y;
          else return x;}
        , MPI_COMM_WORLD);
    //tuple_t next_el = mxx::left_shift(*begin, MPI_COMM_WORLD);

    MP_TIMER_END_SECTION("reductions");

    // for each range/bucket
    for(; begin != end; ) {
      // find bucket (linearly)
      auto eqr = findRange(begin, end, *begin, pc_comp);
      assert(eqr.first == begin);

      // get smallest Pn in bucket
      auto min_pn = std::get<Pn>(*eqr.first);
      if (rank > 0 && std::get<Pc>(prev_min) == std::get<Pc>(*eqr.first)) {
        // first bucket and it starts on the processor to the left
        min_pn = std::get<Pn>(prev_min);
      }

      // get largest Pn in bucket
      auto max_pn = std::get<Pn>(*(eqr.second-1));
      if (rank < p-1 && std::get<Pc>(next_max) == std::get<Pc>(*eqr.first)) {
          max_pn = std::get<Pn>(next_max);
      }

      // remove single element buckets
      if (eqr.first + 1 == eqr.second && (rank == 0
          || (rank > 0 && std::get<Pc>(*eqr.first) != std::get<Pc>(prev_el)))) {
        // single element -> no need to stick around
        std::get<Pc>(*eqr.first) = std::get<Pn>(*eqr.first);
        begin = eqr.second;
        continue;
      }

      // check if all elements of the bucket are identical -> set them to
      // their Pn.
      if (min_pn == max_pn) {
          for (auto it = eqr.first; it != eqr.second; ++it) {
            std::get<Pc>(*it) = std::get<Pn>(*it);
          }
          begin = eqr.second;
          continue;
      }

      done = false;

      // for each further element:
      bool found_flip = false;
      uint32_t prev_pn = std::get<Pn>(prev_el);
      auto it = eqr.first;
      if (rank == 0 || (rank > 0 && std::get<Pc>(*eqr.first) != std::get<Pc>(prev_el))) {
          // skip first (since it is the min entry
          prev_pn = min_pn;
          it++;
      }
      for (; it != eqr.second; ++it) {
        uint32_t next_pn = std::get<Pn>(*it);
        // if duplicate or both entries are equal (Pn==Pc)
        if (std::get<Pn>(*it) == prev_pn || std::get<Pn>(*it) == std::get<Pc>(*it)) {
          if (!found_flip) {
            // set flipped
            found_flip = true;
            std::get<Pn>(*it) = std::get<Pc>(*it);
            std::get<Pc>(*it) = min_pn;
          } else {
            // moves over to new partition (since duplicates are not needed)
            std::get<Pn>(*it) = min_pn;
            std::get<Pc>(*it) = min_pn;
          }
        } else {
          // if either Pn > Pc or Pn < Pc, we flip the entry and return it
          // to `Pn` with the new minimum
          // update tuple, set new min and flip
          std::swap(std::get<Pn>(*it),std::get<Pc>(*it));
          std::get<Pn>(*it) = min_pn;
        }
        prev_pn = next_pn;
      }

      if (!found_flip) {
        // TODO: don't do this for the first or last bucket...
        // TODO: we need only one flipped per bucket
        tuple_t t = *eqr.first;
        std::swap(std::get<Pn>(t),std::get<Pc>(t));
        newtuples.push_back(t);
      }

      // next range
      begin = eqr.second;
    }
    MP_TIMER_END_SECTION("local flips");
    localVector.insert(localVector.end(), newtuples.begin(), newtuples.end());
    MP_TIMER_END_SECTION("vector inserts");

    // check if all processors are done
    keepGoing = !mxx::test_all(done);
    MP_TIMER_END_SECTION("check termination");

    countIterations++;
    if(!rank)
      std::cout << "[RANK 0] : Iteration # " << countIterations <<"\n";
  }


  double time = t.elapsed() - startTime;


    if(!rank)
    {
      std::cout << "Algorithm took " << countIterations << " iteration.\n";
      std::cout << "TOTAL TIME : " << time << " ms.\n"; 
    }

    // get the seeds,
    auto seeds = get_partition_seeds(localVector, MPI_COMM_WORLD);
    if (p > 1)
      // gather the seeds
      mxx::gather_vectors(seeds, MPI_COMM_WORLD).swap(seeds);

    // rank 0 write out the seeds
    if (rank == 0) {
      std::string seedFile = cmdLineVals.seedFile + "." + cmdLineVals.method;

      std::ofstream ofs(seedFile.c_str());
      std::for_each(seeds.begin(), seeds.end(), [&ofs](tuple_t const& x) { ofs << std::get<kmerTuple::Pc>(x) << std::endl; });

      ofs.close();
      printf("partition count = %lu. seeds written to %s\n", seeds.size(), seedFile.c_str());
    }
}

// parallel/MPI log(D_max) implementation with removal of inactive partitions
void cluster_reads_par_inactive(cmdLineParamsGraph500 &cmdLineVals, bool load_balance)
{
  static constexpr int Pn = kmerTuple::Pn;
  static constexpr int Pc = kmerTuple::Pc;
  //Specify Kmer Type
  static constexpr uint32_t inactive_partition = MAX;


  //Define tuple type
  typedef typename Graph500Generator::tuple_type tuple_t;

  // get communicaiton parameters
  int rank, p;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &p);


  if(!rank) {
    std::cout << "Runnning with " << p << " processors.\n";
    std::cout << "Scale : " <<  cmdLineVals.scale << "\n";
    std::cout << "EdgeFactor : " <<  cmdLineVals.edgefactor << "\n";
  }


  mxx::timer t;
  double startTime = t.elapsed();

  /*
   * Indices inside tuple will go like this:
   * 0 : KmerId
   * 1 : P_new
   * 2 : P_old
   */

  MP_TIMER_START();

  // Populate localVector for each rank and return the vector with all the tuples
  std::vector<tuple_t> localVector;

  //Read all the kmers without any filter
  Graph500Generator::generate(cmdLineVals, localVector);

  MP_TIMER_END_SECTION("Generating Data");



  assert(localVector.size() > 0);
  auto start = localVector.begin();
  auto end = localVector.end();
  auto pend = end;

  //Sort tuples by KmerId
  bool keepGoing = true;
  int countIterations = 0;

  // sort by k-mers and update Pn
  mxx::sort(start, pend, layer_comparator<kmerTuple::kmer, tuple_t>(), MPI_COMM_WORLD, false);
  typedef KmerReduceAndMarkAsInactive<tuple_t> KmerReducerType;
  KmerReducerType r1;
  r1(start, pend, MPI_COMM_WORLD);
  MP_TIMER_END_SECTION("iteration KMER phase completed");

  // remove the MAX thing after k-mer reduce:
  // TODO: own k-mer reduction function!
  for (auto it = localVector.begin(); it != localVector.end(); ++it)
  {
    if (std::get<Pn>(*it) >= std::numeric_limits<int>::max()-Pn)
      std::get<Pn>(*it) = std::get<Pc>(*it);
  }

  while (keepGoing) {
    mxx::sort(localVector.begin(), pend,
        [](const tuple_t& x, const tuple_t&y){
        return (std::get<Pc>(x) < std::get<Pc>(y)
            || (std::get<Pc>(x) == std::get<Pc>(y)
            && std::get<Pn>(x) < std::get<Pn>(y)));
        }, MPI_COMM_WORLD, false);
    MP_TIMER_END_SECTION("mxx::sort");

    layer_comparator<Pc, tuple_t> pc_comp;

    // scan and find new p
    auto begin = localVector.begin();
    //auto end = localVector.end();
    std::vector<tuple_t> newtuples;
    bool done = true;

    // TODO: if a local size becomes 0, then there might be an issue with the
    // min/max reductions:

    // TODO: something is still wrong!
    std::size_t local_size = pend - localVector.begin();
    std::vector<std::size_t> distr = mxx::allgather(local_size, MPI_COMM_WORLD);
    if (rank == 0) {
      //std::cout << "local_sizes: [MAX : " << *std::max_element(distr.begin(), distr.end()) << ", MIN: " << *std::min_element(distr.begin(), distr.end()) << ", SUM: " << std::accumulate(distr.begin(), distr.end(), 0) << "]" << std::endl;
      //std::cout << "local_sizes: " << distr << std::endl;
    }

    int color = local_size == 0 ? 0 : 1;
    MPI_Comm nonempty_comm;
    MPI_Comm_split(MPI_COMM_WORLD, color, rank, &nonempty_comm);
    int active_rank, active_p;
    MPI_Comm_rank(nonempty_comm, &active_rank);
    MPI_Comm_size(nonempty_comm, &active_p);

    tuple_t last_min, prev_min, prev_el, first_max, next_max, next_el;
    if (local_size != 0) {
      // find last bucket start and send across boundaries!
      last_min = *(pend-1);
      last_min = *std::lower_bound(begin, pend, last_min, pc_comp);
      // for each processor, get the first element of the last most element
      prev_min = mxx::exscan(last_min,
          [](const tuple_t& x, const tuple_t& y){
            // return max Pc, if equal, return min Pn
            if (std::get<Pc>(x) < std::get<Pc>(y) ||
              (std::get<Pc>(x) == std::get<Pc>(y)
               && std::get<Pn>(x) > std::get<Pn>(y)))
            return y;
            else return x;}
          , nonempty_comm);
      prev_el = mxx::right_shift(*(pend-1), nonempty_comm);

      // get the next element
      first_max = *(std::upper_bound(begin, pend, *begin, pc_comp)-1);
      next_max = mxx::reverse_exscan(first_max,
          [](const tuple_t& x, const tuple_t& y){
            // return min Pc, if equal, return max Pn
            if (std::get<Pc>(x) > std::get<Pc>(y) ||
              (std::get<Pc>(x) == std::get<Pc>(y)
               && std::get<Pn>(x) < std::get<Pn>(y)))
            return y;
            else return x;}
          , nonempty_comm);
      next_el = mxx::left_shift(*begin, nonempty_comm);
    }

    MPI_Comm_free(&nonempty_comm);

    MP_TIMER_END_SECTION("reductions");

    // for each range/bucket
    for(; begin != pend; ) {
      // find bucket (linearly)
      auto eqr = findRange(begin, pend, *begin, pc_comp);
      assert(eqr.first == begin);

      // get smallest Pn in bucket
      auto min_pn = std::get<Pn>(*eqr.first);
      if (active_rank > 0 && std::get<Pc>(prev_min) == std::get<Pc>(*eqr.first)) {
        // first bucket and it starts on the processor to the left
        min_pn = std::get<Pn>(prev_min);
      }

      // get largest Pn in bucket
      auto max_pn = std::get<Pn>(*(eqr.second-1));
      if (active_rank < active_p-1 && std::get<Pc>(next_max) == std::get<Pc>(*eqr.first)) {
          max_pn = std::get<Pn>(next_max);
      }

      // remove single element buckets
      if (eqr.first + 1 == eqr.second && (active_rank == 0
          || (active_rank > 0 && std::get<Pc>(*eqr.first) != std::get<Pc>(prev_el)))) {
        // single element -> no need to stick around
        if (std::get<Pn>(*eqr.first) == inactive_partition-1)
          //std::get<Pn>(*eqr.first) = std::get<Pc>(*eqr.first);
          std::get<Pn>(*eqr.first) = inactive_partition;
        else
          std::get<Pc>(*eqr.first) = std::get<Pn>(*eqr.first);
        begin = eqr.second;
        continue;
      }

      // check if all elements of the bucket are identical -> set them to
      // their Pn.
      if (min_pn == max_pn) {
          if (max_pn == inactive_partition-1)
          {
            // finished!
            for (auto it = eqr.first; it != eqr.second; ++it) {
              std::get<Pn>(*it) = inactive_partition;
            }
          }
          else if (std::get<Pc>(*eqr.first) == max_pn) {
            // finished, but need to participate in one more round
            // with one of my tuples
            for (auto it = eqr.first; it != eqr.second; ++it) {
              std::get<Pn>(*it) = inactive_partition-1;
            }
          } else {
            for (auto it = eqr.first; it != eqr.second; ++it) {
              std::get<Pc>(*it) = std::get<Pn>(*it);
            }
          }
          begin = eqr.second;
          continue;
      }

      if (min_pn > std::get<Pc>(*eqr.first))
        min_pn = std::get<Pc>(*eqr.first);

      done = false;

      // for each further element:
      bool found_flip = false;
      uint32_t prev_pn = std::get<Pn>(prev_el);
      auto it = eqr.first;
      if (rank == 0 || (rank > 0 && std::get<Pc>(*eqr.first) != std::get<Pc>(prev_el))) {
          if (std::get<Pn>(*eqr.first) > min_pn)
            std::get<Pn>(*eqr.first) = min_pn;
          // skip first (since it is the min entry
          prev_pn = min_pn;
          it++;
      }
      for (; it != eqr.second; ++it) {
        if (std::get<Pn>(*it) == inactive_partition-1)
          std::get<Pn>(*it) = std::get<Pc>(*it);
        uint32_t next_pn = std::get<Pn>(*it);
        // if duplicate or both entries are equal (Pn==Pc)
        if (std::get<Pn>(*it) == prev_pn || std::get<Pn>(*it) == std::get<Pc>(*it)) {
          if (!found_flip) {
            // set flipped
            found_flip = true;
            std::get<Pn>(*it) = std::get<Pc>(*it);
            std::get<Pc>(*it) = min_pn;
          } else {
            // moves over to new partition (since duplicates are not needed)
            std::get<Pn>(*it) = min_pn;
            std::get<Pc>(*it) = min_pn;
          }
        } else {
          // if either Pn > Pc or Pn < Pc, we flip the entry and return it
          // to `Pn` with the new minimum
          // update tuple, set new min and flip
          std::swap(std::get<Pn>(*it),std::get<Pc>(*it));
          std::get<Pn>(*it) = min_pn;
        }
        prev_pn = next_pn;
      }

      if (!found_flip) {
        // TODO: don't do this for the first or last bucket...
        // TODO: we need only one flipped per bucket
        tuple_t t = *eqr.first;
        std::swap(std::get<Pn>(t),std::get<Pc>(t));
        newtuples.push_back(t);
      }

      // next range
      begin = eqr.second;
    }


    MP_TIMER_END_SECTION("local flips");
    std::size_t nnew = newtuples.size();
    // insert at end and swap into active part of vector
    std::size_t active_size = pend - localVector.begin();
    std::size_t inactive_size = localVector.end() - pend;
    localVector.insert(localVector.end(), newtuples.begin(), newtuples.end());
    std::size_t nswap = std::min(nnew, inactive_size);
    for (std::size_t i = 0; i < nswap; ++i) {
        std::swap(localVector[active_size+i], localVector[localVector.size()-1-i]);
    }
    pend = localVector.begin() + active_size + nnew;
    MP_TIMER_END_SECTION("vector inserts");

    pend = std::partition(localVector.begin(), pend, [](tuple_t& t){return std::get<Pn>(t) != inactive_partition;});
    MP_TIMER_END_SECTION("std::partition");

    // load balance
    if (load_balance)
      pend = mxx::block_decompose_partitions(localVector.begin(), pend, localVector.end(), MPI_COMM_WORLD);

    // check if all processors are done
    keepGoing = !mxx::test_all(done);
    MP_TIMER_END_SECTION("check termination");

    countIterations++;
    if(!rank)
      std::cout << "[RANK 0] : Iteration # " << countIterations <<"\n";
  }

  double time = t.elapsed() - startTime;


    if(!rank)
    {
      std::cout << "Algorithm took " << countIterations << " iteration.\n";
      std::cout << "TOTAL TIME : " << time << " ms.\n"; 
    }

    // get the seeds,
    auto seeds = get_partition_seeds(localVector, MPI_COMM_WORLD);

    // gather the seeds
    if (p > 1)
      mxx::gather_vectors(seeds, MPI_COMM_WORLD).swap(seeds);

    // rank 0 write out the seeds
    if (rank == 0) {
      std::string seedFile = cmdLineVals.seedFile + "." + cmdLineVals.method;

      std::ofstream ofs(seedFile.c_str());
      std::for_each(seeds.begin(), seeds.end(), [&ofs](tuple_t const& x) { ofs << std::get<kmerTuple::Pc>(x) << std::endl; });

      ofs.close();
      printf("partition count = %lu. seeds written to %s\n", seeds.size(), seedFile.c_str());

    }
}

int main(int argc, char** argv)
{
  // Initialize the MPI library:
  MPI_Init(&argc, &argv);

  // get communicaiton parameters
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  //Parse command line arguments
  ArgvParser cmd;
  cmdLineParamsGraph500 cmdLineVals;

  cmd.setIntroductoryDescription("Parallel partitioning algorithm used for benchmarking (SC15)");
  cmd.setHelpOption("h", "help", "Print this help page");

  cmd.defineOption("scale", "scale of graph for Graph500 generator = log(num of vertices)", ArgvParser::OptionRequiresValue | ArgvParser::OptionRequired);
  cmd.defineOption("edgefactor", "average edge degree for vertex for Graph500 generator", ArgvParser::OptionRequiresValue | ArgvParser::OptionRequired);
  cmd.defineOption("method", "Type of log-sort to run (standard[Naive], inactive[AP], loadbalance[AP_LB])", ArgvParser::OptionRequiresValue | ArgvParser::OptionRequired);
  cmd.defineOption("seedfile", "file to write out the seed for each component.", ArgvParser::OptionRequiresValue | ArgvParser::OptionRequired);

  int result = cmd.parse(argc, argv);

  if (result != ArgvParser::NoParserError)
  {
    if (!rank) cout << cmd.parseErrorDescription(result) << "\n";
    exit(1);
  }

  cmdLineVals.scale = atol(cmd.optionValue("scale").c_str());
  cmdLineVals.edgefactor = atol(cmd.optionValue("edgefactor").c_str());
  cmdLineVals.method = cmd.optionValue("method"); 
  cmdLineVals.seedFile = cmd.optionValue("seedfile");

  if (cmdLineVals.method == "standard")
    cluster_reads_par(cmdLineVals);
  else if (cmdLineVals.method == "inactive")
    cluster_reads_par_inactive(cmdLineVals, false);
  else if (cmdLineVals.method == "loadbalance")
    cluster_reads_par_inactive(cmdLineVals, true);
  else {
    std::cout << "Usage: mpirun -np 4 <executable> --method <method> --file FASTQ_FILE\n";
    std::cout << "  where <method> can be: \"standard\" (Naive), \"inactive\"(AP) ,\"loadbalance\"(AP_LB)\n";
    return 1;
  }

  MPI_Finalize();
  return(0);
}


