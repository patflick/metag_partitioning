//Includes
#include <mpi.h>
#include <iostream> 

//File includes from BLISS
#include <common/kmer.hpp>
#include <common/base_types.hpp>

//Own includes
#include "sortTuples.hpp"
#include "parallel_fastq_iterate.hpp"

int main(int argc, char** argv)
{
  // Initialize the MPI library:
  MPI_Init(&argc, &argv);

  //Specify the fileName
  std::string filename; 
  if( argc == 2 ) {
    filename = argv[1];
  }
  else {
    std::cout << "Usage: mpirun -np 4 <executable> FASTQ_FILE\n";
    return 1;
  }

  //Specify Kmer Type
  const int kmerLength = 31;
  typedef bliss::common::DNA AlphabetType;

  typedef bliss::common::Kmer<kmerLength, AlphabetType, uint64_t> KmerType;

  //Assuming kmer-length is less than 32
  typedef uint64_t KmerIdType;

  //Assuming read count is less than 4 Billion
  typedef uint32_t ReadIdType;

  //Know rank
  int rank, commsize;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &commsize);

  if(!rank)
  {
    std::cout << "Runnning with " << commsize<< " processors.\n"; 
    std::cout << "Filename : " <<  filename << "\n"; 
  }
  

  //Initialize the KmerVector
  /*
   * Indices inside tuple will go like this:
   * 0 : KmerId
   * 1 : P_new
   * 2 : P_old
   */
  typedef typename std::tuple<KmerIdType, ReadIdType, ReadIdType> tuple_t;
  std::vector<tuple_t> localVector;

  //Populate localVector for each rank and return the vector with all the tuples
  generateReadKmerVector<KmerType, AlphabetType, ReadIdType> (filename, localVector); 

  //Sort tuples by KmerId
  bool keepGoing = true;
  int countIterations = 0;

  while(keepGoing)
  {
    //Sort by Kmers
    //Update P_n
    sortTuples<0,1,false> (localVector);

    //keepGoing will be updated here
    bool localKeepGoing;

    //Sort by P_c
    //Update P_n and P_c both
    sortTuples<2,1,true> (localVector, localKeepGoing);

    //Check whether all processors are done
    MPI_Allreduce(&localKeepGoing, &keepGoing, 1, MPI_CHAR , MPI_MAX, MPI_COMM_WORLD);
    countIterations++;
    if(!rank)
      std::cout << "[RANK 0] : Iteration # " << countIterations <<"\n";
  }

  //printTuples(localVector);


  if(!rank)
    std::cout << "Algorithm took " << countIterations << " iteration.\n"; 

  MPI_Finalize();   
  return(0);

}


