
//@HEADER
// ************************************************************************
// 
//               HPCG: Simple Conjugate Gradient Benchmark Code
// Questions? Contact Michael A. Heroux (maherou@sandia.gov) 
// 
// ************************************************************************
//@HEADER

#if defined(DEBUG) || defined(DETAILEDDEBUG)
#include <fstream>
using std::endl;
#include "hpcg.hpp"
#include <cstdlib>
#include <cstdio>
#include <cassert>
#endif

#include "SetupHalo.hpp"
#include "mytimer.hpp"

#ifndef HPCG_NOMPI
#include <mpi.h>
#include <map>
#include <set>
#endif

#ifndef HPCG_NOOPENMP
#include <omp.h>
#endif

void SetupHalo(const Geometry & geom, SparseMatrix & A) {

	double t0;
#ifdef DEBUG
#ifdef DETAILEDDEBUG
	int debug_details = 1; // Set to 1 for voluminous output
#else
	int debug_details = 0; // Set to 0
#endif
	int debug = 1;
#else
	int debug = 0;
#endif

	// Extract Matrix pieces

	local_int_t localNumberOfRows = A.localNumberOfRows;
	char  * nonzerosInRow = A.nonzerosInRow;
	global_int_t ** mtxIndG = A.mtxIndG;
	local_int_t ** mtxIndL = A.mtxIndL;

	#ifdef HPCG_NOMPI  // In the non-MPI case we simply copy global indices to local index storage
#ifndef HPCG_NOOPENMP
#pragma omp parallel for
#endif
	for (local_int_t i=0; i< localNumberOfRows; i++) {
		int cur_nnz = nonzerosInRow[i];
		for (int j=0; j<cur_nnz; j++)	mtxIndL[i][j] = mtxIndG[i][j];
	}

#else // Run this section if compiling for MPI

	// Scan global IDs of the nonzeros in the matrix.  Determine if the column ID matches a row ID.  If not:
	// 1) We call the getRankOfMatrixRow function, which tells us the rank of the processor owning the row ID.
	//	We need to receive this value of the x vector during the halo exchange.
	// 2) We record our row ID since we know that the other processor will need this value from us, due to symmetry.

	std::map< int, std::set< global_int_t> > sendList, receiveList;
	typedef std::map< int, std::set< global_int_t> >::iterator map_iter;
	typedef std::set<global_int_t>::iterator set_iter;
	std::map< local_int_t, local_int_t > externalToLocalMap;

	// TODO: With proper critical and atomic regions, this loop could be threaded, but not attempting it at this time
	for (local_int_t i=0; i< localNumberOfRows; i++) {
		global_int_t currentGlobalRow = A.localToGlobalMap[i];
		for (int j=0; j<nonzerosInRow[i]; j++) {
			global_int_t curIndex = mtxIndG[i][j];
			int rankIdOfColumnEntry = getRankOfMatrixRow(geom, A, curIndex);
#ifdef DETAILEDDEBUG
			HPCG_fout << "rank, row , col, globalToLocalMap[col] = " << geom.rank << " " << currentGlobalRow << " "
					<< curIndex << " " << A.globalToLocalMap[curIndex] << endl;
#endif
			if (geom.rank!=rankIdOfColumnEntry) {// If column index is not a row index, then it comes from another processor
				receiveList[rankIdOfColumnEntry].insert(curIndex);
				sendList[rankIdOfColumnEntry].insert(currentGlobalRow); // Matrix symmetry means we know the neighbor process wants my value
			}
		}
	}

	// Count number of matrix entries to send and receive
	local_int_t totalToBeSent = 0;
	for (map_iter curNeighbor = sendList.begin(); curNeighbor != sendList.end(); ++curNeighbor) {
		totalToBeSent += (curNeighbor->second).size();
	}
	local_int_t totalToBeReceived = 0;
	for (map_iter curNeighbor = receiveList.begin(); curNeighbor != receiveList.end(); ++curNeighbor) {
		totalToBeReceived += (curNeighbor->second).size();
	}

#ifdef DEBUG
	// These are all attributes that should be true, due to symmetry
	if (debug_details) HPCG_fout << "totalToBeSent = " << totalToBeSent << " totalToBeReceived = " << totalToBeReceived << endl;
	assert(totalToBeSent==totalToBeReceived); // Number of sent entry should equal number of received
	assert(sendList.size()==receiveList.size()); // Number of send-to neighbors should equal number of receive-from
	// Each receive-from neighbor should be a send-to neighbor, and send the same number of entries
	for (map_iter curNeighbor = receiveList.begin(); curNeighbor != receiveList.end(); ++curNeighbor) {
		assert(sendList.find(curNeighbor->first)!=sendList.end());
		assert(sendList[curNeighbor->first].size()==receiveList[curNeighbor->first].size());
	}
#endif

	// Build the arrays and lists needed by the ExchangeHalo function.
	double * sendBuffer = new double[totalToBeSent];
	local_int_t * elementsToSend = new local_int_t[totalToBeSent];
	int * neighbors = new int[sendList.size()];
	local_int_t * receiveLength = new local_int_t[receiveList.size()];
	local_int_t * sendLength = new local_int_t[sendList.size()];
	int neighborCount = 0;
	local_int_t receiveEntryCount = 0;
	local_int_t sendEntryCount = 0;
	for (map_iter curNeighbor = receiveList.begin(); curNeighbor != receiveList.end(); ++curNeighbor, ++neighborCount) {
		int neighborId = curNeighbor->first; // rank of current neighbor we are processing
		neighbors[neighborCount] = neighborId; // store rank ID of current neighbor
		receiveLength[neighborCount] = receiveList[neighborId].size();
		sendLength[neighborCount] = sendList[neighborId].size(); // Get count if sends/receives
		for (set_iter i = receiveList[neighborId].begin(); i != receiveList[neighborId].end(); ++i, ++receiveEntryCount) {
			externalToLocalMap[*i] = localNumberOfRows + receiveEntryCount; // The remote columns are indexed at end of internals
		}
		for (set_iter i = sendList[neighborId].begin(); i != sendList[neighborId].end(); ++i, ++sendEntryCount) {
			//if (geom.rank==1) HPCG_fout << "*i, globalToLocalMap[*i], sendEntryCount = " << *i << " " << A.globalToLocalMap[*i] << " " << sendEntryCount << endl;
			elementsToSend[sendEntryCount] = A.globalToLocalMap[*i]; // store local ids of entry to send
		}
	}

	// Convert matrix indices to local IDs
#ifndef HPCG_NOOPENMP
#pragma omp parallel for
#endif
	for (local_int_t i=0; i< localNumberOfRows; i++) {
		for (int j=0; j<nonzerosInRow[i]; j++) {
			global_int_t curIndex = mtxIndG[i][j];
			int rankIdOfColumnEntry = getRankOfMatrixRow(geom, A, curIndex);
			if (geom.rank==rankIdOfColumnEntry) { // My column index, so convert to local index
				mtxIndL[i][j] = A.globalToLocalMap[curIndex];
			}
			else { // If column index is not a row index, then it comes from another processor
				mtxIndL[i][j] = externalToLocalMap[curIndex];
			}
		}
	}

	// Store contents in our matrix struct
	A.numberOfExternalValues = externalToLocalMap.size();
	A.localNumberOfColumns = A.localNumberOfRows + A.numberOfExternalValues;
	A.numberOfSendNeighbors = sendList.size();
	A.totalToBeSent = totalToBeSent;
	A.elementsToSend = elementsToSend;
	A.neighbors = neighbors;
	A.receiveLength = receiveLength;
	A.sendLength = sendLength;
	A.sendBuffer = sendBuffer;

#ifdef DEBUG
	HPCG_fout << " For rank " << geom.rank << " of " << geom.size << ", number of neighbors = " << A.numberOfSendNeighbors << endl;
	for (int i = 0; i < A.numberOfSendNeighbors; i++) {
		HPCG_fout << "     rank " << geom.rank << " neighbor " << neighbors[i] << " send/recv length = " << sendLength[i] << "/" << receiveLength[i] << endl;
#ifdef DETAILEDDEBUG
		for (local_int_t j = 0; j<sendLength[i]; ++j)
			HPCG_fout << "       rank " << geom.rank << " elementsToSend[" << j << "] = " << elementsToSend[j] << endl;
#endif
	}
#endif

#endif // ifndef HPCG_NOMPI

	return;
}