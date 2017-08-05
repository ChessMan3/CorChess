/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cstring>   // For std::memset
#include <iostream>
#include <fstream>

#include "bitboard.h"
#include "tt.h"
#include "uci.h"
#include "windows.h"

TranspositionTable TT; // Our global transposition table
int use_large_pages = -1;
int got_privileges = -1;


bool Get_LockMemory_Privileges()
{
    HANDLE TH, PROC7;
    TOKEN_PRIVILEGES tp;
    bool ret = false;

    PROC7 = GetCurrentProcess();
    if (OpenProcessToken(PROC7, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &TH))
    {
        if (LookupPrivilegeValue(NULL, TEXT("SeLockMemoryPrivilege"), &tp.Privileges[0].Luid))
        {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (AdjustTokenPrivileges(TH, FALSE, &tp, 0, NULL, 0))
            {
                if (GetLastError() != ERROR_NOT_ALL_ASSIGNED)
                    ret = true;
            }
        }
        CloseHandle(TH);
    }
    return ret;
}


void Try_Get_LockMemory_Privileges()
{
    use_large_pages = 0;

    if (Options["Large Pages"] == false)    
        return;

    if (got_privileges == -1)
    {
        if (Get_LockMemory_Privileges() == true)
            got_privileges = 1;
        else
        {
            sync_cout << "No Privilege for Large Pages" << sync_endl;
            got_privileges = 0;
        }
    }

    if (got_privileges == 0)      
        return;

    use_large_pages = 1;        
}


/// TranspositionTable::resize() sets the size of the transposition table,
/// measured in megabytes. Transposition table consists of a power of 2 number
/// of clusters and each cluster consists of ClusterSize number of TTEntry.

void TranspositionTable::resize(size_t mbSize) {

  if (mbSize == 0)
      mbSize = mbSize_last_used;

  if (mbSize == 0)
      return;

  mbSize_last_used = mbSize;

  Try_Get_LockMemory_Privileges();

  size_t newClusterCount = size_t(1) << msb((mbSize * 1024 * 1024) / sizeof(Cluster));

  if (newClusterCount == clusterCount)
  {
      if ((use_large_pages == 1) && (large_pages_used))      
          return;
      if ((use_large_pages == 0) && (large_pages_used == false))
          return;
  }

  clusterCount = newClusterCount;
 
  if (use_large_pages < 1)
  {
      if (mem != NULL)
      {
          if (large_pages_used)
              VirtualFree(mem, 0, MEM_RELEASE);
          else          
              free(mem);
      }
      uint64_t memsize = clusterCount * sizeof(Cluster) + CacheLineSize - 1;
      mem = calloc(memsize, 1);
      large_pages_used = false;
  }
  else
  {
      if (mem != NULL)
      {
          if (large_pages_used)
              VirtualFree(mem, 0, MEM_RELEASE);
          else
              free(mem);
      }

      int64_t memsize = clusterCount * sizeof(Cluster);
      mem = VirtualAlloc(NULL, memsize, MEM_LARGE_PAGES | MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (mem == NULL)
      {
          std::cerr << "Failed to allocate " << mbSize
              << "MB Large Page Memory for transposition table, switching to default" << std::endl;

          use_large_pages = 0;
          memsize = clusterCount * sizeof(Cluster) + CacheLineSize - 1;
          mem = calloc(memsize, 1);
          large_pages_used = false;
      }
      else
      {
          sync_cout << "info string LargePages " << (memsize >> 20) << " Mb" << sync_endl;
          large_pages_used = true;
      }
        
  }

  if (!mem)
  {
      std::cerr << "Failed to allocate " << mbSize
                << "MB for transposition table." << std::endl;
      exit(EXIT_FAILURE);
  }

  table = (Cluster*)((uintptr_t(mem) + CacheLineSize - 1) & ~(CacheLineSize - 1));
}


/// TranspositionTable::clear() overwrites the entire transposition table
/// with zeros. It is called whenever the table is resized, or when the
/// user asks the program to clear the table (from the UCI interface).

void TranspositionTable::clear() {

  std::memset(table, 0, clusterCount * sizeof(Cluster));
}

void TranspositionTable::set_hash_file_name(const std::string& fname) { hashfilename = fname; }

bool TranspositionTable::save() {
	std::ofstream b_stream(hashfilename,
		std::fstream::out | std::fstream::binary);
	if (b_stream)
	{
		b_stream.write(reinterpret_cast<char const *>(table), clusterCount * sizeof(Cluster));
		return (b_stream.good());
	}
	return false;
}

void TranspositionTable::load() {
	std::ifstream file(hashfilename, std::ios::binary | std::ios::ate);
	std::streamsize size = file.tellg();
	resize(size / 1024 / 1024);
	file.seekg(0, std::ios::beg);
	file.read(reinterpret_cast<char *>(table), clusterCount * sizeof(Cluster));
}
/// TranspositionTable::probe() looks up the current position in the transposition
/// table. It returns true and a pointer to the TTEntry if the position is found.
/// Otherwise, it returns false and a pointer to an empty or least valuable TTEntry
/// to be replaced later. The replace value of an entry is calculated as its depth
/// minus 8 times its relative age. TTEntry t1 is considered more valuable than
/// TTEntry t2 if its replace value is greater than that of t2.

TTEntry* TranspositionTable::probe(const Key key, bool& found) const {

  TTEntry* const tte = first_entry(key);
  const uint16_t key16 = key >> 48;  // Use the high 16 bits as key inside the cluster

  for (int i = 0; i < ClusterSize; ++i)
      if (!tte[i].key16 || tte[i].key16 == key16)
      {
          if ((tte[i].genBound8 & 0xFC) != generation8 && tte[i].key16)
              tte[i].genBound8 += 4; // Refresh

          return found = (bool)tte[i].key16, &tte[i];
      }

  // Find an entry to be replaced according to the replacement strategy
  TTEntry* replace = tte;
  for (int i = 1; i < ClusterSize; ++i)
      // Due to our packed storage format for generation and its cyclic
      // nature we add 259 (256 is the modulus plus 3 to keep the lowest
      // two bound bits from affecting the result) to calculate the entry
      // age correctly even after generation8 overflows into the next cycle.
      if (  replace->depth8 - ((259 + generation8 - replace->genBound8) & 0xFC) * 2
          >   tte[i].depth8 - ((259 + generation8 -   tte[i].genBound8) & 0xFC) * 2)
          replace = &tte[i];

  return found = false, replace;
}


/// TranspositionTable::hashfull() returns an approximation of the hashtable
/// occupation during a search. The hash is x permill full, as per UCI protocol.

int TranspositionTable::hashfull() const {

  int cnt = 0;
  for (int i = 0; i < 1000 / ClusterSize; i++)
  {
      const TTEntry* tte = &table[i].entry[0];
      for (int j = 0; j < ClusterSize; j++)
          if ((tte[j].genBound8 & 0xFC) == generation8)
              cnt++;
  }
  return cnt;
}
