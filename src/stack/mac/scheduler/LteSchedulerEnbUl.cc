// 
//                           SimuLTE
// Copyright (C) 2012 Antonio Virdis, Daniele Migliorini, Giovanni
// Accongiagioco, Generoso Pagano, Vincenzo Pii.
// 
// This file is part of a software released under the license included in file
// "license.pdf". This license can be also found at http://www.ltesimulator.com/
// The above file and the present reference are part of the software itself, 
// and cannot be removed from it.
// 

#include "LteSchedulerEnbUl.h"
#include "LteMacEnb.h"
#include "LteHarqBufferRx.h"
#include "LteAllocationModule.h"


// TODO
bool
LteSchedulerEnbUl::checkEligibility(MacNodeId id, Codeword& cw)
{
    try {
            // check if harq buffer have already been created for this node
        if (mac_->getHarqRxBuffers()->find(id)!= mac_->getHarqRxBuffers()->end())
        {
            LteHarqBufferRx* ulHarq = mac_->getHarqRxBuffers()->at(id);

            // get current Harq Process for nodeId
            unsigned char currentAcid = harqStatus_.at(id);
            // get current Harq Process status
            std::vector<RxUnitStatus> status = ulHarq->getProcess(currentAcid)->getProcessStatus();
            // check if at least one codeword buffer is available for reception
            for(; cw < MAX_CODEWORDS; ++cw)
            {
                if (status.at(cw).second == RXHARQ_PDU_EMPTY)
                {
                    return true;
                }
            }
        }
    }
    catch (Codeword)
    {
        throw cRuntimeError("EXCEPTION! Exception in LteSchedulerEnbUl::checkEligibility, abnormal codeword id.");
    }
    return false;
}

void
LteSchedulerEnbUl::updateHarqDescs()
{

    EV << NOW << "LteSchedulerEnbUl::updateHarqDescs  cell " << mac_->getMacCellId() << endl;

    HarqRxBuffers::iterator it;
    HarqStatus::iterator currentStatus;

    for (it=harqRxBuffers_->begin();it!=harqRxBuffers_->end();++it)
    {
        if ((currentStatus=harqStatus_.find(it->first)) != harqStatus_.end())
        {
            EV << NOW << "LteSchedulerEnbUl::updateHarqDescs UE " << it->first << " OLD Current Process is  " << (unsigned int)currentStatus->second << endl;
            // updating current acid id
            currentStatus->second = (currentStatus->second +1 ) % (it->second->getProcesses());

            EV << NOW << "LteSchedulerEnbUl::updateHarqDescs UE " << it->first << "NEW Current Process is " << (unsigned int)currentStatus->second << "(total harq processes " <<  it->second->getProcesses() << ")"<< endl;
        }
        else
        {
            EV << NOW << "LteSchedulerEnbUl::updateHarqDescs UE " << it->first << " initialized the H-ARQ status " << endl;
            harqStatus_[it->first]=0;
        }

    }
}

bool LteSchedulerEnbUl::racschedule()
{
    EV << NOW << " LteSchedulerEnbUl::racschedule --------------------::[ START RAC-SCHEDULE ]::--------------------" << endl;
    EV << NOW << " LteSchedulerEnbUl::racschedule eNodeB: " <<  mac_->getMacCellId() << endl;
    EV << NOW << " LteSchedulerEnbUl::racschedule Direction: " <<  (direction_ == UL ? "UL" : "DL") << endl;

    RacStatus::iterator it=racStatus_.begin() , et=racStatus_.end();

    for (;it!=et;++it)
    {
        // get current nodeId
        MacNodeId nodeId = it->first;
        EV << NOW << " LteSchedulerEnbUl::racschedule handling RAC for node " <<  nodeId << endl;

        // Get number of logical bands
        unsigned int numBands = mac_->getDeployer()->getNumBands();

        // FIXME default behavior
        //try to allocate one block to selected UE on at least one logical band of MACRO antenna, first codeword

        const unsigned int cw =0;
        const unsigned int blocks =1;

        bool allocation=false;

        for (Band b=0;b<numBands;++b)
        {
            if ( allocator_->availableBlocks(nodeId,MACRO,b) >0)
            {

                allocator_->addBlocks(MACRO,b,nodeId,1, mac_->getAmc()->computeBytesOnNRbs(nodeId,b,cw,blocks,UL) );

                EV<<NOW<< "LteSchedulerEnbUl::racschedule UE: " << nodeId << "Handled RAC on band: "<< b << endl;

                allocation=true;
                break;
            }
        }

        if (allocation)
        {
            // create scList id for current node/codeword
            std::pair<unsigned int,Codeword> scListId = std::pair<unsigned int,Codeword>(nodeId,cw);

            scheduleList_[scListId]=blocks;
        }
    }

    // clean up all requests
    racStatus_.clear();

    EV <<NOW << " LteSchedulerEnbUl::racschedule --------------------::[  END RAC-SCHEDULE  ]::--------------------"<< endl;

    int availableBlocks = allocator_->computeTotalRbs();

    return (availableBlocks==0);

}

bool
LteSchedulerEnbUl::rtxschedule() {

    // try to handle RAC requests first and abort rtx scheduling if no OFDMA space is left after
    if (racschedule()) return true;

    try {
        EV << NOW << " LteSchedulerEnbUl::rtxschedule --------------------::[ START RTX-SCHEDULE ]::--------------------" << endl;
        EV << NOW << " LteSchedulerEnbUl::rtxschedule eNodeB: " <<  mac_->getMacCellId() << endl;
        EV << NOW << " LteSchedulerEnbUl::rtxschedule Direction: " <<  (direction_ == UL ? "UL" : "DL") << endl;

        HarqRxBuffers::iterator it= harqRxBuffers_->begin() , et=harqRxBuffers_->end();

        for(; it != et; ++it) {
            // get current nodeId
            MacNodeId nodeId = it->first;

            // get current Harq Process for nodeId
            unsigned char currentAcid = harqStatus_.at(nodeId);

            EV<<NOW<< "LteSchedulerEnbUl::rtxschedule UE: " << nodeId << "Acid: "<<(unsigned int)currentAcid << endl;

            // Get user transmission parameters
            const UserTxParams& txParams = mac_->getAmc()->computeTxParams(nodeId, direction_);    // get the user info


            unsigned int codewords = txParams.getLayers().size();                // get the number of available codewords
            unsigned int allocatedBytes =0;

            // TODO handle the codewords join case (sizeof(cw0+cw1) < currentTbs && currentLayers ==1)

            for(Codeword cw = 0; (cw < MAX_CODEWORDS) && (codewords>0) ; ++cw)
            {
                unsigned int rtxBytes=0;
                // FIXME PERFORMANCE: check for rtx status before calling rtxAcid

                // perform a retransmission on available codewords for the selected acid
                rtxBytes=LteSchedulerEnbUl::schedulePerAcidRtx(nodeId, cw,currentAcid);
                if (rtxBytes>0)
                {
                    --codewords;
                    allocatedBytes+=rtxBytes;
                }
            }
            EV <<NOW << "LteSchedulerEnbUl::rtxschedule user "<< nodeId <<" allocated bytes : "<< allocatedBytes << endl;
        }

        int availableBlocks = allocator_->computeTotalRbs();

        EV <<NOW << " LteSchedulerEnbUl::rtxschedule residual OFDM Space: " << availableBlocks << endl ;

        EV <<NOW << " LteSchedulerEnbUl::rtxschedule --------------------::[  END RTX-SCHEDULE  ]::--------------------"<< endl;


        return (availableBlocks == 0);

     }
     catch(std::out_of_range)
     {
        throw cRuntimeError("EXCEPTION! Exception in LteSchedulerEnbUl::rtxschedule");
     }
     return 0;
}

unsigned int
LteSchedulerEnbUl::schedulePerAcidRtx(MacNodeId nodeId, Codeword cw, unsigned char acid, std::vector<BandLimit>* bandLim, Remote antenna,bool limitBl) {
    try {
        std::string bands_msg="BAND_LIMIT_SPECIFIED";
        if(bandLim==NULL)
        {
            bands_msg = "NO_BAND_SPECIFIED";
            // Create a vector of band limit using all bands
            // FIXME: bandlim is never deleted
            bandLim = new std::vector< BandLimit > ();

            unsigned int numBands = mac_->getDeployer()->getNumBands();
            // for each band of the band vector provided
            for(unsigned int i=0; i<numBands; i++)
            {
                BandLimit elem;
                // copy the band
                elem.band_ = Band(i);
                EV << "Putting band " << i << endl;
                // mark as unlimited
                for (Codeword i=0;i<MAX_CODEWORDS;++i)
                {
                    elem.limit_.push_back(-1);
                }
                bandLim->push_back(elem);
            }
        }

        EV << NOW << "LteSchedulerEnbUl::rtxAcid - Node["<<mac_->getMacNodeId() << ", User["<< nodeId<<", Codeword[ " << cw<<"], ACID["<<(unsigned int)acid<<"] "<< endl;

        // Get the current active HARQ process
//        unsigned char currentAcid = harqStatus_.at(nodeId) ;

        unsigned char currentAcid = (harqStatus_.at(nodeId) + 2)%(harqRxBuffers_->at(nodeId)->getProcesses());
        EV << "\t the acid that should be considered is " << currentAcid << endl;

        // acid e currentAcid sono identici per forza, dato che sono calcolati nello stesso modo
//        if(acid != currentAcid)
//        {        // If requested HARQ process is not current for TTI
//            EV << NOW << " LteSchedulerEnbUl::rtxAcid User is on ACID " << (unsigned int)currentAcid << " while requested one is " << (unsigned int)acid <<". No RTX scheduled. " << endl;
//            return 0;
//        }

        LteHarqProcessRx* currentProcess = harqRxBuffers_->at(nodeId)->getProcess(currentAcid);

        if( currentProcess->getUnitStatus(cw) != RXHARQ_PDU_CORRUPTED )
        {    // exit if the current active HARQ process is not ready for retransmission
            EV << NOW << " LteSchedulerEnbUl::rtxAcid User is on ACID " << (unsigned int)currentAcid << " HARQ process is IDLE. No RTX scheduled ." << endl;
            delete(bandLim);
            return 0;
        }

        Codeword allocatedCw = 0;
//        Codeword allocatedCw = MAX_CODEWORDS;
        // search for already allocated codeword
        // create "mirror" scList ID for other codeword than current
        std::pair<unsigned int,Codeword> scListMirrorId = std::pair<unsigned int,Codeword>(nodeId,MAX_CODEWORDS-cw-1);
        if (scheduleList_.find(scListMirrorId)!=scheduleList_.end())
        {
            allocatedCw=MAX_CODEWORDS-cw-1;
        }
        // get current process buffered PDU byte length
        unsigned int bytes =   currentProcess->getByteLength(cw);
        // bytes to serve
        unsigned int toServe = bytes;
        // blocks to allocate for each band
        std::vector<unsigned int> assignedBlocks;
        // bytes which blocks from the preceding vector are supposed to satisfy
        std::vector<unsigned int> assignedBytes;

        // end loop signal [same as bytes>0, but more secure]
        bool finish=false;
        // for each band
        unsigned int size = bandLim->size();
        for(unsigned int i = 0; (i < size ) && (!finish); ++i)
        {
            // save the band and the relative limit
            Band b = bandLim->at(i).band_;
            int limit = bandLim->at(i).limit_.at(cw);

            // TODO add support to multi CW
//            unsigned int bandAvailableBytes = // if a codeword has been already scheduled for retransmission, limit available blocks to what's been  allocated on that codeword
//                    ((allocatedCw == MAX_CODEWORDS) ? availableBytes(nodeId,antenna, b, cw) : mac_->getAmc()->blocks2bytes(nodeId, b, cw, allocator_->getBlocks(antenna,b,nodeId) , direction_));    // available space
            unsigned int bandAvailableBytes = availableBytes(nodeId,antenna, b, cw);

            // use the provided limit as cap for available bytes, if it is not set to unlimited
            if(limit >= 0)
                bandAvailableBytes =  limit < (int) bandAvailableBytes ? limit : bandAvailableBytes;

            EV << NOW <<  " LteSchedulerEnbUl::rtxAcid BAND " <<  b << endl;
            EV << NOW <<  " LteSchedulerEnbUl::rtxAcid total bytes:" << bytes << " still to serve: " << toServe <<" bytes" << endl;
            EV << NOW <<  " LteSchedulerEnbUl::rtxAcid Available: " << bandAvailableBytes <<" bytes" << endl;

            unsigned int servedBytes=0;
            // there's no room on current band for serving the entire request
            if(bandAvailableBytes < toServe)
            {
                // record the amount of served bytes
                servedBytes= bandAvailableBytes;
            // the request can be fully satisfied
            } else {
                // record the amount of served bytes
                servedBytes = toServe;
                // signal end loop - all data have been serviced
                finish=true;
            }
            unsigned int servedBlocks = mac_->getAmc()->computeReqRbs(nodeId, b, cw, servedBytes, direction_);
            // update the bytes counter
            toServe -= servedBytes;
            // update the structures
            assignedBlocks.push_back(servedBlocks);
            assignedBytes.push_back(servedBytes);
        }

        if(toServe > 0)
        {
            // process couldn't be served - no sufficient space on available bands
            EV <<NOW <<" LteSchedulerEnbUl::rtxAcid Unavailable space for serving node " << nodeId << " ,HARQ Process " << (unsigned int)currentAcid << " on codeword " << cw << endl;
            delete(bandLim);
            return 0;
        } else
        {
            // record the allocation
            unsigned int size = assignedBlocks.size();
            unsigned int cwAllocatedBlocks =0;

            // create scList id for current node/codeword
            std::pair<unsigned int,Codeword> scListId = std::pair<unsigned int,Codeword>(nodeId,cw);

            for(unsigned int i = 0; i < size; ++i)
            {    // For each LB for which blocks have been allocated
                Band b = bandLim->at(i).band_;

                cwAllocatedBlocks +=assignedBlocks.at(i);
                EV << "\t Cw->" << allocatedCw << "/" << MAX_CODEWORDS << endl;
                //! handle multi-codeword allocation
                if (allocatedCw!=MAX_CODEWORDS)
                {
                    EV <<NOW <<" LteSchedulerEnbUl::rtxAcid - adding " << assignedBlocks.at(i) << " to band " << i << endl;
                    allocator_->addBlocks(antenna,b,nodeId,assignedBlocks.at(i),assignedBytes.at(i));
                }
                //! TODO check if ok bandLim->at.limit_.at(cw) = assignedBytes.at(i);
            }

            // signal a retransmission
            // schedule list contains number of granted blocks

            scheduleList_[scListId] = cwAllocatedBlocks;
            // mark codeword as used
            if (allocatedCws_.find(nodeId)!=allocatedCws_.end())
            {
                allocatedCws_.at(nodeId)++;
            } else
            {
                allocatedCws_[nodeId]=1;
            }

            EV <<NOW <<" LteSchedulerEnbUl::rtxAcid HARQ Process " << (unsigned int)currentAcid << " : " << bytes << " bytes served! " << endl;

            delete(bandLim);
            return bytes;
        }
    }
    catch(std::out_of_range)
    {
        throw cRuntimeError("EXCEPTION! Exception in LteSchedulerEnbUl::rtxAcid.");
    }
    delete(bandLim);
    return 0;
}

void LteSchedulerEnbUl::initHarqStatus(MacNodeId id , unsigned char acid)
{
    EV << NOW <<")LteSchedulerEnbUl::initHarqStatus - initializing harq status for id " << id << "to status " << (unsigned int)acid << endl;
    harqStatus_[id]=acid;
}



