			STEC EnhanceIO SSD Caching Software
				25th December, 2012


1. WHAT IS ENHANCEIO?

	EnhanceIO driver is based on EnhanceIO SSD caching software product 
	developed by STEC Inc. EnhanceIO was derived from Facebook's open source
	Flashcache project. EnhanceIO uses SSDs as cache devices for
	traditional rotating hard disk drives (referred to as source volumes
	throughout this document).

	EnhanceIO can work with any block device, be it an entire physical
	disk, an individual disk partition,  a RAIDed DAS device, a SAN volume,
	a device mapper volume or a software RAID (md) device.

	The source volume to SSD mapping is a set-associative mapping based on
	the source volume sector number with a default set size
	(aka associativity) of 512 blocks and a default block size of 4 KB.
	Partial cache blocks are not used. The default value of 4 KB is chosen
	because it is the common I/O block size of most storage systems.  With
	these default values, each cache set is 2 MB (512 * 4 KB).  Therefore,
	a 400 GB SSD will have a little less than 200,000 cache sets because a
	little space is used for storing the meta data on the SSD.

	EnhanceIO supports three caching modes: read-only, write-through, and
	write-back and three cache replacement policies: random, FIFO, and LRU.

	Read-only caching mode causes EnhanceIO to direct write IO requests only
	to HDD. Read IO requests are issued to HDD and the data read from HDD is
	stored on SSD. Subsequent Read requests for the same blocks are carried
	out from SSD, thus reducing their latency by a substantial amount. 

	In Write-through mode - reads are handled similar to Read-only mode.
	Write-through mode causes EnhanceIO to write application data to both
	HDD and SSD. Subsequent reads of the same data benefit because they can
	be served from SSD.

	Write-back improves write latency by writing application requested data
	only to SSD. This data, referred to as dirty data, is copied later to
	HDD asynchronously. Reads are handled similar to Read-only and
	Write-through modes.

2. WHAT HAS ENHANCEIO CHANGED TO FLASHCACHE?

2.1. A new write-back engine

	The write-back engine in EnhanceiO has been designed from scratch.
	Several optimizations have been done. IO completion guarantees have
	been improved. We have defined limits to let a user control the amount
	of dirty data in a cache. Clean-up of dirty data is stopped by default
	under a high load; this can be overridden if required. A user can
	control the extent to which a single cache set can be filled with dirty
	data. A background thread cleans-up dirty data at regular intervals.
	Clean-up is also done at regular intevals by identifying cache sets
	which have been written least recently.

2.2. Transparent cache

	EnhanceIO does not use device mapper. This enables creation and
	deletion of caches while a source volume is being used. It's possible
	to either create or delete cache while a partition is mounted.

	EnhanceIO also supports creation of a cache for a device which contains
	partitions. With this feature it's possible to create a cache without
	worrying about having to create several SSD partitions and many
	separate caches.


2.3. Large I/O Support

	Unlike Flashcache, EnhanceIO does not cause source volume I/O requests
	to be split into cache block size pieces. For the typical SSD cache
	block size of 4 KB, this means that a write I/O request size of, say,
	64 KB to the source volume is not split into 16 individual requests of
	4 KB each. This is a performance improvement over Flashcache. IO
	codepaths have been substantially modified for this improvement.

2.4. Small Memory Footprint

	Through a special compression algorithm, the meta data RAM usage has
	been reduced to only 4 bytes for each SSD cache block (versus 16 bytes
	in Flashcache).  Since the most typical SSD cache block size is 4 KB,
	this means that RAM usage is 0.1% (1/1000) of SSD capacity.
	For example, for a 400 GB SSD, EnhanceIO will need only 400 MB to keep
	all meta data in RAM.

	For an SSD cache block size of 8 KB, RAM usage is 0.05% (1/2000) of SSD
	capacity.

	The compression algorithm needs at least 32,768 cache sets
	(i.e., 16 bits to encode the set number). If the SSD capacity is small
	and there are not at least 32,768 cache sets, EnhanceIO uses 8 bytes of
	RAM for each SSD cache block. In this case, RAM usage is 0.2% (2/1000)
	of SSD capacity for a cache block size of 4K.

2.5. Loadable Replacement Policies

	Since the SSD cache size is typically 10%-20% of the source volume
	size, the set-associative nature of EnhanceIO necessitates cache
	block replacement.

	The main EnhanceIO kernel module that implements the caching engine
	uses a random (actually, almost like round-robin) replacement policy
	that does not require any additional RAM and has the least CPU
	overhead.  However, there are two additional kernel modules that
	implement FIFO and LRU replacement policies.  FIFO is the default cache
	replacement policy because it uses less RAM than LRU.  The FIFO and LRU
	kernel modules are independent of each other and do not have to be
	loaded if they are not needed.

	Since the replacement policy modules do not consume much RAM when not
	used, both modules are typically loaded after the main caching engine
	is loaded. RAM is used only after a cache has been instantiated to use
	either the FIFO or the LRU replacement policy.

	Please note that the RAM used for replacement policies is in addition
	to the RAM used for meta data (mentioned in Section 2.1).  The table
	below shows how	much RAM each cache replacement policy uses:

		POLICY	RAM USAGE
		------	---------
		Random	0
		FIFO	4 bytes per cache set
		LRU	4 bytes per cache set + 4 bytes per cache block

2.6. Optimal Alignment of Data Blocks on SSD

	EnhanceIO writes all meta data and data blocks on 4K-aligned blocks
	on the SSD. This minimizes write amplification and flash wear.
	It also improves performance.

2.7. Improved device failure handling

	Failure of an SSD device in read-only and write-through modes is
	handled gracefully by allowing I/O to continue to/from the
	source volume. An application may notice a drop in performance but it
	will not receive any I/O errors.

	Failure of an SSD device in write-back mode obviously results in the
	loss of dirty blocks in the cache. To guard against this data loss, two
	SSD devices can be mirrored via RAID 1.

	EnhanceIO identifies device failures based on error codes. Depending on
	whether the failure is likely to be intermittent or permanent, it takes
	the best suited action.

2.8. Coding optimizations

	Several coding optizations have been done to reduce CPU usage.  These
	include removing queues which are not required for write-through and
	read-only cache modes, splitting of a single large spinlock, and more.
	Most of the code paths in flashcache have been substantially
	restructured.

2.9 Sequential I/O bypass

	EnhanceIO has removed the bypass of sequential IO available in flashcache.
	The sequential detection logic has a limited use case, espescially in a
	reasonably multithreaded scenario.


3. EnhanceIO usage

3.1. Cache creation, deletion and editing properties

	eio_cli utility is used for creating and deleting caches and editing
	their properties. Manpage for this utility eio_cli(8) provides more
	information.

3.2. Making a cache configuration persistent
	It's essential that a cache be resumed before any applications or a
	filesystem use the source volume during a bootup. If a cache is enabled
	after a source volume is written to, stale data may be present in the
	cache. It may cause data corruption. The document Persistent.txt
	describes how to enable a cache during bootup using udev scripts.

	In case an SSD does not come up during a bootup, it's ok to allow read
	and write access to HDD only in the case of a Write-through or a
	read-only cache. A cache should be created again when SSD becomes
	available. If a previous cache configuration is resumed, it may cause
	stale data to be read.

3.3. Using a Write-back cache
	It's absolutely necessary to make a Write-back cache configuration
	persistent. This is required particularly in the case of an OS crash or
	a power failure.  A Write-back cache may contain dirty blocks which
	haven't been written to HDD yet. Reading the source volume without
	enabling the cache will cause incorrect data to be read.

	In case an SSD does not come up during a bootup, access to HDD should
	stopped. It should be enabled only after SSD comes-up and a cache is
	enabled.
	
	Write-back cache needs to perform clean operation in order to flush the
	dirty data to the source device(HDD). The clean can be either trigerred
	by the user or automatically initiated, based on preconfigured
	thresholds. These thresholds are described below. They can be set using 
	sysctl calls.

	a) Dirty high threshold (%) : The upper limit on percentage of dirty
	   blocks in the entire cache.
	b) Dirty low threshold (%) : The lower limit on percentage of dirty
	   blocks in the entire cache.
	c) Dirty set high threshold (%) : The upper limit on percentage of dirty
	   blocks in a set.
	d) Dirty set low threshold (%) : The lower limit on percentage of dirty
	   blocks in a set.	
	e) Automatic clean-up threshold : An automatic clean-up of the cache
	   will occur only if the number of outstanding I/O requests from the
	   HDD is below the threshold.
	f) Time based clean-up interval (minutes) : This option allows you to
	   specify an interval between each clean-up process.

	Clean is trigerred when one of the upper thresholds or time based clean 
	threshold is met and stops when all the lower thresholds are met.  


4. ACKNOWLEDGEMENTS

	STEC acknowledges Facebook and in particular Mohan Srinivasan
	for the design, development, and release of Flashcache as an
	open source project.

	Flashcache, in turn, is based on DM-Cache by Ming Zhao.
