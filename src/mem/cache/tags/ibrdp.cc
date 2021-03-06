/*
 * Copyright (C) Pavlos Petoumenos <ppetoumenos@ece.upatras.gr>
 * Copyright (c) 2015 Min Cai <min.cai.china@gmail.com>
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2003-2005,2014 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Pavlos Petoumenos, Min Cai
 */

/**
 * @file
 * Definitions of a IBRD tag store.
 */

#include "debug/CacheRepl.hh"
#include "mem/cache/tags/ibrdp.hh"
#include "mem/cache/base.hh"

IbRDP::IbRDP(const Params *p)
    : BaseSetAssoc(p)
{
    accessesCounterLow  = 0;
    accessesCounterHigh = 1;
    predictor = new IBRDPredictor( IBRDP_SETS, IBRDP_WAYS );
    rdsampler = new RDSampler( SAMPLER_PERIOD, SAMPLER_MAX_RD, predictor );
}

CacheBlk*
IbRDP::accessBlock(ThreadID threadId, Addr pc, Addr addr, bool is_secure, Cycles &lat, int master_id)
{
    CacheBlk *blk = BaseSetAssoc::accessBlock(threadId, pc, addr, is_secure, lat, master_id);

    if (blk != NULL) {
        UpdateIBRDP( blk->set, blk->way, pc, true );
    }

    return blk;
}

CacheBlk*
IbRDP::findVictim(ThreadID threadId, Addr pc, Addr addr)
{
    CacheBlk *blk = BaseSetAssoc::findVictim(threadId, pc, addr);
    int set = extractSet(addr);

    // if all blocks are valid, pick a replacement by the algorithm
    if (blk && blk->isValid()) {
        blk = findBlockBySetAndWay(set, Get_IBRDP_Victim( set, pc, addr ));
    }

    return blk;
}

void
IbRDP::insertBlock(PacketPtr pkt, BlkType *blk)
{
    BaseSetAssoc::insertBlock(pkt, blk);

    int set = extractSet(pkt->getAddr());
    Addr pc = pkt->req->hasPC() ?
        pkt->req->getPC() : 0;

    UpdateIBRDP( set, blk->way, pc, false );
}

void
IbRDP::invalidate(CacheBlk *blk)
{
    BaseSetAssoc::invalidate(blk);
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function finds the IBRDP victim in the cache set                      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
int
IbRDP::Get_IBRDP_Victim( uint32_t setIndex, Addr PC, Addr paddr )
{
    uint32_t way;
                                // All local variables hold unquantized values
    uint32_t now;                 // Current time
    uint32_t timestamp;           // The line's time of last use
    uint32_t prediction;          // The line's reuse distance prediction
    uint32_t time_left;           // The line's predicted time left until reuse
    uint32_t time_idle;           // The line's elapsed time since last use

    int victim_way  = 0;      // The line which was used farthest in the past
                                // or will be used farthest in the future
    uint32_t victim_time = 0;     // The idle or left time for the victim_way

    // We search the set to find the line which will be used farthest in
    // the future / was used farthest in the past
    for( way = 0; way < assoc; way++ )
    {
        // ---> Un-Quantize all the needed variables <---

        // 'timestamp' refers to a point in the past, so it should be less
        // than 'accessesCounterHigh'. If this is not the case, it means
        // that the accesses counter has overflowed since the last access,
        // so we have to add to accessesCounterHigh 'MAX_VALUE_TIMESTAMP+1'
        CacheBlk *blk = findBlockBySetAndWay(setIndex, way);

        if( blk->timestamp > accessesCounterHigh )
            now = UnQuantizeTimestamp( accessesCounterHigh + MAX_VALUE_TIMESTAMP + 1 );
        else
            now = UnQuantizeTimestamp( accessesCounterHigh );

        timestamp  = UnQuantizeTimestamp( blk->timestamp );
        prediction = UnQuantizePrediction( blk->prediction );

        // ---> Look at the future <---

        // Calculate Time Left until next access
        if( timestamp + prediction > now )
            time_left = timestamp + prediction - now;
        else
            time_left = 0;

        // If the line is going to be used farther in the future than the
        // previously selected victim, then we replace the selected victim
        if( time_left > victim_time )
        {
            victim_time = time_left;
            victim_way  = way;
        }

        // ---> Look at the past <---

        // Calculate time passed since last access
        time_idle = now - timestamp;

        // If the line was used farther in the past than the previously
        // selected victim, then we replace the selected victim
        if( time_idle > victim_time )
        {
            victim_time = time_idle;
            victim_way  = way;
        }
    }

    return victim_way;

}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function implements the IBRDP update routine                          //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void
IbRDP::UpdateIBRDP( uint32_t setIndex, int updateWayID, Addr PC, bool cacheHit )
{
    uint32_t prediction = 0;
    uint32_t myPC = TransformPC( PC );

    CacheBlk *blk = findBlockBySetAndWay(setIndex, updateWayID);

    uint32_t myAddress = TransformAddress( ( blk->tag << setShift ) + setIndex );

    // Update the accesses counter and the sampler
    UpdateOnEveryAccess( myAddress, myPC );

    // Get the prediction information for the accessed line
    prediction = predictor->Lookup( myPC );

    // Fill the accessed line with the replacement policy information
    blk->timestamp  = accessesCounterHigh;
    blk->prediction = prediction;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function updates the IBRDP elements that must be updated              //
// for every access: the accessesCounter and the the Sampler                  //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void
IbRDP::UpdateOnEveryAccess( uint32_t address, uint32_t PC )
{
    accessesCounterLow++;
    if( accessesCounterLow == QUANTUM_TIMESTAMP )
    {
        accessesCounterLow = 0;
        accessesCounterHigh++;
        if( accessesCounterHigh > MAX_VALUE_TIMESTAMP )
            accessesCounterHigh = 0;
    }
    rdsampler->Update( address, PC );
}

IbRDP*
IbRDPParams::create()
{
    return new IbRDP(this);
}

//---------------------------------------------------------------------------///
//---------------------------------------------------------------------------///
//                         REUSE DISTANCE SAMPLER                            ///
//---------------------------------------------------------------------------///
//---------------------------------------------------------------------------///

////////////////////////////////////////////////////////////////////////////////
// _max_rd is always 1 larger than the longest reuse distance not truncated   //
// due to the limited width of the prediction, that is equal to:              //
// (MAX_VALUE_PREDICTION + 1) * QUANTUM_PREDICTION                            //
// Based on that the RDSampler allocates enough entries so that it holds      //
// each sample for a time equal to _max_rd cache accesses                     //
////////////////////////////////////////////////////////////////////////////////
RDSampler::RDSampler( uint32_t _period, uint32_t _max_rd, IBRDPredictor *_predictor )
{
    uint32_t i;

    period = _period;
    size   = _max_rd / _period;
    predictor = _predictor;

    sampling_counter = 0;

    sampler = new RDSamplerEntry[ size ];
    assert(sampler != NULL);

    // Initialize entries
    for( i = 0; i < size; i++ )
    {
        sampler[i].valid = 0;
        sampler[i].FifoPosition = i;
    }
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function updates the Sampler. It searches for a previously taken      //
// sample for the currently accessed address and if it finds one it updates   //
// the predictor. Also it checks whether we should take a new sample.         //
// When we take a sample, if the oldest (soon to be evicted) entry is still   //
// valid, its reuse distance is longer than the MAX_VALUE_PREDICTION so we    //
// update the predictor with this maximum value, even though we don't know    //
// its exact non-quantized reuse distance.                                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void
RDSampler::Update( uint32_t address, uint32_t pc )
{
    uint32_t observation;
    uint32_t position;
    uint32_t index;
    uint32_t j;

    //
    // ---> Match <---
    //

    // Search the sampler for a previous sample of this address
    // Stop when we've checked all entries or when we've found a previous sample
    for( index = 0; index < size; index++ )
        if( ( sampler[index].valid ) && ( sampler[index].address == address ) )
            break;

    // If we found a sample, invalidate the entry, determine the observed
    // reuse distance and update the predictor
    // Optimization: penalize stores by artificially increasing their positions
    if( index < size )
    {
        sampler[index].valid = 0;

        position = sampler[index].FifoPosition;

        observation = QuantizePrediction( position * period );
        predictor->Update( sampler[index].pc, observation );
    }

    //
    // ---> Sample <---
    //

    // It's time for a new sample?
    if( sampling_counter == 0 )
    {
        // Get the oldest entry
        for( index = 0; index < size; index++ )
            if( sampler[index].FifoPosition == ( size - 1 ) )
                break;

        // If the oldest entry is still valid, update the
        // predictor with the maximum prediction value
        if( sampler[index].valid == 1 )
            predictor->Update( sampler[index].pc, MAX_VALUE_PREDICTION );

        // Update the FIFO Queue
        for( j = 0; j < size; j++ )
            sampler[j].FifoPosition++;

        // Fill the new entry
        sampler[index].valid = 1;
        sampler[index].FifoPosition = 0;
        sampler[index].pc = pc;
        sampler[index].address = address;

        sampling_counter = period - 1;
    }
    else
    {
        sampling_counter--;
    }
}

//---------------------------------------------------------------------------///
//---------------------------------------------------------------------------///
//               INSTRUCTION BASED REUSE DISTANCE PREDICTOR                  ///
//---------------------------------------------------------------------------///
//---------------------------------------------------------------------------///
IBRDPredictor::IBRDPredictor( uint32_t _numsets, uint32_t _assoc )
{
    uint32_t set, way;

    numsets   = _numsets;
    assoc     = _assoc;
    set_mask  = numsets - 1;
    set_shift = CRC_FloorLog2( numsets );

    predictor = new IBRDP_Entry* [ numsets ];
    assert(predictor != NULL);

    for( set = 0; set < numsets; set++ )
    {
        predictor[set] = new IBRDP_Entry [ assoc ];
        assert(predictor[set] != NULL);
        for( way = 0; way < assoc; way++ )
        {
            predictor[set][way].valid = 0;
            predictor[set][way].tag = 0;
            predictor[set][way].prediction = 0;
            predictor[set][way].confidence = 0;
            predictor[set][way].StackPosition = way;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Lookup searches for an IBRDPredictor entry for the given PC.               //
// If it finds one, it returns the prediction stored in the entry.            //
// If not, it returns -1.                                                     //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
uint32_t
IBRDPredictor::Lookup( uint32_t pc )
{
    uint32_t set, way;
    uint32_t prediction = 0;

    set = pc & set_mask;
    way = FindEntry( pc );

    if( way != assoc )
        if( predictor[set][way].confidence >= SAFE_CONFIDENCE )
            prediction = predictor[set][way].prediction;

    return prediction;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Update finds the entry associated with the given pc, or allocates a new one//
// and then it updates its prediction: If the observation is equal to the     //
// prediction, it increases the confidence in our prediction. If the          //
// observation is different than the prediction, it decreases the confidence. //
// If the confidence is already zero, then we replace the prediction          //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void
IBRDPredictor::Update( uint32_t pc, uint32_t observation )
{
    uint32_t set, way;

    set = pc & set_mask;
    way = FindEntry( pc );

    // If no entry was found, get a new one, and initialize it
    if( way == assoc )
    {
        way = GetEntry( pc );
        predictor[set][way].prediction = observation;
        predictor[set][way].confidence = 0;
    }
    // else update the entry
    else
    {
        if( predictor[set][way].prediction == observation )
        {
            if ( predictor[set][way].confidence < MAX_CONFIDENCE )
                predictor[set][way].confidence++;
        }
        else
        {
            if( predictor[set][way].confidence == 0 )
                predictor[set][way].prediction = observation;
            else
                predictor[set][way].confidence--;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// FindEntry searches the predictor to find an entry associated with the      //
// given PC. Afterwards it updates the LRU StackPositions of the entries.     //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
uint32_t
IBRDPredictor::FindEntry( uint32_t pc )
{
    uint32_t myway, way;
    uint32_t set   = pc & set_mask;
    uint32_t tag   = pc >> set_shift;

    // Search the set, to find a matching entry
    for( way = 0; way < assoc; way++ )
        if( predictor[set][way].tag == tag )
            break;

    myway = way;

    // If we found an entry, update the LRU Stack Positions
    if( myway != assoc )
    {
        for( way = 0; way < assoc; way++)
            if( predictor[set][way].StackPosition < predictor[set][myway].StackPosition)
                predictor[set][way].StackPosition++;

        predictor[set][myway].StackPosition = 0;
    }

    return myway;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// GetEntry is called when we want to allocate a new entry. It searches for   //
// the LRU Element in the list, and re-initializes it.                        //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
uint32_t
IBRDPredictor::GetEntry( uint32_t pc )
{
    uint32_t way;
    uint32_t myway = assoc;
    uint32_t set   = pc & set_mask;
    uint32_t tag   = pc >> set_shift;

    // Search the set to find the LRU entry
    // At the same time, update the LRU Stack Positions
    for( way = 0; way < assoc ; way++ )
    {
        if( predictor[set][way].StackPosition == ( assoc - 1 ) )
            myway = way;
        else
            predictor[set][way].StackPosition++;
    }
    assert( myway != assoc );

    // Initialize the new entry
    predictor[set][myway].valid = 1;
    predictor[set][myway].tag = tag;
    predictor[set][myway].StackPosition = 0;

    return myway;
}
