#include "Transposition.h"

#include <string>
#include <fstream>

#include "BitBoard.h"
#include "Engine.h"

Transposition::Table TT;

namespace Transposition {

    using namespace std;

    u08 Entry::Generation;

    // Size of Transposition entry (10 bytes)
    static_assert (sizeof (Entry) == 10, "Entry size incorrect");
    // Size of Transposition cluster (32 bytes)
    static_assert (CacheLineSize % sizeof (Cluster) == 0, "Cluster size incorrect");

    // Alocate the aligned memory
    void Table::alloc_aligned_memory (size_t mem_size, u32 alignment)
    {
        assert((alignment & (alignment-1)) == 0);
        assert((mem_size  & (alignment-1)) == 0);

    #if defined(LPAGES)

        Memory::alloc_memory (mem, mem_size, alignment);
        if (nullptr == mem)
        {
            return;
        }
        clusters = reinterpret_cast<Cluster*> ((uintptr_t(mem) + alignment-1) & ~uintptr_t(alignment-1));
        assert((uintptr_t(clusters) & (alignment-1)) == 0);

    #else

        // Need to use malloc provided by C.
        // First need to allocate memory of mem_size + max (alignment, sizeof (void *)).
        // Need 'bytes' because user requested it.
        // Need to add 'alignment' because malloc can give us any address and
        // Need to find multiple of 'alignment', so at maximum multiple
        // of alignment will be 'alignment' bytes away from any location.
        // Need 'sizeof(void *)' for implementing 'aligned_free',
        // since returning modified memory pointer, not given by malloc, to the user,
        // must free the memory allocated by malloc not anything else.
        // So storing address given by malloc just above pointer returning to user.
        // Thats why needed extra space to store that address.
        // Then checking for error returned by malloc, if it returns NULL then 
        // alloc_aligned_memory will fail and return NULL or exit().

        alignment = std::max (u32(sizeof (void *)), alignment);

        mem = malloc (mem_size + alignment-1);
        if (nullptr == mem)
        {
            std::cerr << "ERROR: Hash memory allocate failed " << (mem_size >> 20) << " MB" << std::endl;
            return;
        }
        sync_cout << "info string Hash " << (mem_size >> 20) << " MB" << sync_endl;
        clusters = reinterpret_cast<Cluster*> ((uintptr_t(mem) + alignment-1) & ~uintptr_t(alignment-1));
        assert((uintptr_t(clusters) & (alignment-1)) == 0);

    #endif

    }
    // Free the aligned memory
    void Table::free_aligned_memory ()
    {
    #   if defined(LPAGES)
        Memory::free_memory (mem);
    #   else
        free (mem);
    #   endif
        mem = nullptr;
        clusters = nullptr;
        cluster_count = 0;
    }

    // resize(mb) sets the size of the table, measured in mega-bytes.
    // Transposition table consists of a power of 2 number of clusters and
    // each cluster consists of Cluster::EntryCount number of entry.
    u32 Table::resize (u32 mem_size, bool force)
    {
        mem_size = std::min (std::max (mem_size, MinHashSize), MaxHashSize);
        size_t msize = size_t(mem_size) << 20;
        u08 hash_bit = BitBoard::scan_msq (msize / sizeof (Cluster));
        assert(hash_bit <= MaxHashBit);

        size_t new_cluster_count = size_t(1) << hash_bit;

        msize = new_cluster_count * sizeof (Cluster);
        if (   force
            || cluster_count != new_cluster_count)
        {
            free_aligned_memory ();
            alloc_aligned_memory (msize, CacheLineSize);
        }

        if (nullptr == clusters)
        {
            cluster_count = 0;
            return 0;
        }
        else
        {
            cluster_count = new_cluster_count;
            clear ();
            return u32(msize >> 20);
        }
    }
    u32 Table::resize ()
    {
        return resize (size (), true);
    }

    void Table::auto_resize (u32 mem_size, bool force)
    {
        for (auto msize =
             0 != mem_size ?
                mem_size :
                MaxHashSize;
             msize >= MinHashSize;
             msize /= 2)
        {
            if (0 != resize (msize, force))
            {
                return;
            }
        }
        Engine::stop (EXIT_FAILURE);
    }
    // Clear the entire transposition table.
    void Table::clear ()
    {
        assert(nullptr != clusters);
        // Clear first cluster
        std::memset (clusters, 0x00, sizeof (*clusters));
        for (auto *ite = clusters->entries; ite < clusters->entries + Cluster::EntryCount; ++ite)
        {
            ite->d08 = Entry::Empty;
        }
        // Clear other cluster using first cluster as template
        for (auto *itc = clusters + 1; itc < clusters + cluster_count; ++itc)
        {
            std::memcpy (itc, clusters, sizeof (*clusters));
        }
    }

    // probe() looks up the entry in the transposition table.
    // If the position is found, it returns true and a pointer to the found entry.
    // Otherwise, it returns false and a pointer to an empty or least valuable entry to be replaced later.
    Entry* Table::probe (const Key key, bool &tt_hit) const
    {
        auto *const fte = cluster_entry (key);
        assert(nullptr != fte);
        // Find an entry to be replaced according to the replacement strategy.
        auto *rte = fte; // Default first
        auto rworth = rte->worth ();
        for (auto *ite = fte; ite < fte + Cluster::EntryCount; ++ite)
        {
            if (   ite->empty ()
                || ite->k16 == (key >> 0x30))
            {
                tt_hit = !ite->empty ();
                // Refresh entry.
                if (   tt_hit
                    && ite->generation () != Entry::Generation)
                {
                    ite->gb08 = u08(Entry::Generation + ite->bound ());
                }
                return ite;
            }

            //if (ite == fte) continue;

            // Replacement strategy.
            auto iworth = ite->worth ();
            if (rworth > iworth)
            {
                rworth = iworth;
                rte = ite;
            }
        }
        return tt_hit = false, rte;
    }
    // Returns an approximation of the per-mille of the 
    // all transposition entries during a search which have received
    // at least one write during the current search.
    // It is used to display the "info hashfull ..." information in UCI.
    // "the hash is <x> permill full", the engine should send this info regularly.
    // hash, are using <x>%. of the state of full.
    u32 Table::hash_full () const
    {
        u32 entry_count = 0;
        auto cluster_limit = std::min (size_t(1000 / Cluster::EntryCount), cluster_count);
        for (const auto *itc = clusters; itc < clusters + cluster_limit; ++itc)
        {
            for (const auto *ite = itc->entries; ite < itc->entries + Cluster::EntryCount; ++ite)
            {
                if (ite->generation () == Entry::Generation)
                {
                    ++entry_count;
                }
            }
        }
        return u32(entry_count * 1000 / (cluster_limit * Cluster::EntryCount));
    }

    void Table::save (const string &hash_fn) const
    {
        ofstream ofs (hash_fn, ios_base::out|ios_base::binary);
        if (ofs.is_open ())
        {
            ofs << *this;
            ofs.close ();
            sync_cout << "info string Hash saved to file \'" << hash_fn << "\'" << sync_endl;
        }
    }

    void Table::load (const string &hash_fn)
    {
        ifstream ifs (hash_fn, ios_base::in|ios_base::binary);
        if (ifs.is_open ())
        {
            ifs >> *this;
            ifs.close ();
            sync_cout << "info string Hash loaded from file \'" << hash_fn << "\'" << sync_endl;
        }
    }

}
