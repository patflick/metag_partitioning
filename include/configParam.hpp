#ifndef CONFIG_HPP
#define CONFIG_HPP

/*
 * ADJUSTABLE BY USER
 */

//Histogram equalization threshold
constexpr int HIST_EQ_THRESHOLD = 10; 

//Controls the threshold for read filtering
constexpr int KMER_FREQ_THRESHOLD = 50;

//Kmer length during filtering phase
constexpr int KMER_LEN_PRE = 21;

//Kmer length during de Bruijn graph partitioning
//For now, keep it <= 32
constexpr int KMER_LEN = 31;

//Maximum read size in the dataset
const unsigned int MAX_READ_SIZE=128;





/*
 * NOT SUPPOSED TO BE CHANGED BY USER
 */

//Assuming read count is less than 4 Billion
//NOTE: Not sure about the correctness at the moment if following type is changed
typedef uint32_t ReadIdType;

const unsigned int MAX = std::numeric_limits<ReadIdType>::max();

//Order of layers in kmer tuples (kmer, Pn, Pc)
enum class kmerTuple {kmer,Pn,Pc};

//Order of layers in read sequence tuples (Sequence, readid, partitionid, count of nuc. characters in the read)
enum class readTuple{seq,rid,pid,cnt};

#endif
