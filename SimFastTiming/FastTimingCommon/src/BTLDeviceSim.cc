#include "SimFastTiming/FastTimingCommon/interface/BTLDeviceSim.h"
#include "DataFormats/DetId/interface/DetId.h"
#include "DataFormats/ForwardDetId/interface/MTDDetId.h"
#include "DataFormats/ForwardDetId/interface/BTLDetId.h"

#include "FWCore/Framework/interface/ESHandle.h"
#include "Geometry/CommonDetUnit/interface/GeomDetType.h"
#include "Geometry/MTDGeometryBuilder/interface/ProxyMTDTopology.h"
#include "Geometry/MTDGeometryBuilder/interface/RectangularMTDTopology.h"

#include "CLHEP/Random/RandGaussQ.h"

BTLDeviceSim::BTLDeviceSim(const edm::ParameterSet& pset) : 
  bxTime_(pset.getParameter<double>("bxTime") ),
  LightYield_(pset.getParameter<double>("LightYield")),
  LightCollEff_(pset.getParameter<double>("LightCollectionEff")),
  LightCollTime_(pset.getParameter<double>("LightCollectionTime")),
  smearLightCollTime_(pset.getParameter<double>("smearLightCollectionTime")),
  PDE_(pset.getParameter<double>("PhotonDetectionEff")) { }

void BTLDeviceSim::getEventSetup(const edm::EventSetup& evs) {
  edm::ESHandle<MTDGeometry> geom;
  if( geomwatcher_.check(evs) || geom_ == nullptr ) {
    evs.get<MTDDigiGeometryRecord>().get(geom);
    geom_ = geom.product();
  }
}

void BTLDeviceSim::getHitsResponse(const std::vector<std::tuple<int,uint32_t,float> > &hitRefs, 
				   const edm::Handle<edm::PSimHitContainer> &hits,
				   mtd_digitizer::MTDSimHitDataAccumulator *simHitAccumulator,
				   CLHEP::HepRandomEngine *hre){

  //loop over sorted simHits
  const int nchits = hitRefs.size();
  for(int ihit=0; ihit<nchits; ++ihit) {

    const int hitidx   = std::get<0>(hitRefs[ihit]);
    const uint32_t id  = std::get<1>(hitRefs[ihit]);
    const MTDDetId detId(id);
    const PSimHit &hit = hits->at( hitidx );     
    
    // --- Safety check on the detector ID
    if ( detId.det()!=DetId::Forward || detId.mtdSubDetector()!=1 ) continue;

    if(id==0) continue; // to be ignored at RECO level                                                              
    BTLDetId btlid(detId) ;
    DetId geoId = BTLDetId(btlid.mtdSide(),btlid.mtdRR(),btlid.module()+18*(btlid.modType()-1),0,1);
    const MTDGeomDet* thedet = geom_->idToDet(geoId);
    if( thedet == nullptr ) {
      throw cms::Exception("BTLDeviceSim") << "GeographicalID: " << std::hex
                                           << geoId.rawId()
                                           << " (" << detId.rawId()<< ") is invalid!" << std::dec
                                           << std::endl;
    }
    const ProxyMTDTopology& topoproxy = static_cast<const ProxyMTDTopology&>(thedet->topology());
    const RectangularMTDTopology& topo = static_cast<const RectangularMTDTopology&>(topoproxy.specificTopology());
    // calculate the simhit row and column                                                                           
    const auto& pentry = hit.entryPoint();
    Local3DPoint simscaled(0.1*pentry.x(),0.1*pentry.y(),0.1*pentry.z());
    // translate from crystal-local coordinates to module-local coordinates to get the row and column
    simscaled = topo.pixelToModuleLocalPoint(simscaled,btlid.row(),btlid.column());
    const auto& thepixel = topo.pixel(simscaled); // mm -> cm here is the switch
    const uint8_t row(thepixel.first), col(thepixel.second);

    if( btlid.row() != row || btlid.column() != col ) {
      throw cms::Exception("BTLDeviceSim")
	<< "BTLDetId (row,column): (" << btlid.row() << ',' << btlid.column() <<") is not equal to "
	<< "topology (row,column): (" << uint32_t(row) << ',' << uint32_t(col) <<")";
    }
    
    // --- Store the detector element ID as a key of the MTDSimHitDataAccumulator map
    auto simHitIt = simHitAccumulator->emplace(mtd_digitizer::MTDCellId(id,row,col),
					       mtd_digitizer::MTDCellInfo()).first;

    // --- Get the simHit energy and convert it from MeV to photo-electrons
    float Npe = 1000.*hit.energyLoss()*LightYield_*LightCollEff_*PDE_;

    // --- Get the simHit time of arrival and add the light collection time
    float toa = std::get<2>(hitRefs[ihit]) + LightCollTime_;

    if ( smearLightCollTime_ > 0. )
      toa += CLHEP::RandGaussQ::shoot(hre, 0., smearLightCollTime_);

    // --- Accumulate in 15 buckets of 25 ns (9 pre-samples, 1 in-time, 5 post-samples)
    const int itime = std::floor( toa/bxTime_ ) + 9;
    if(itime<0 || itime>14) continue;     

    // --- Check if the time index is ok and accumulate the energy
    if(itime >= (int)simHitIt->second.hit_info[0].size() ) continue;

    (simHitIt->second).hit_info[0][itime] += Npe;

    // --- Store the time of the first SimHit in the right DataFrame bucket
    const float tof = toa - (itime-9)*bxTime_;

    if( (simHitIt->second).hit_info[1][itime] == 0 ||
	tof < (simHitIt->second).hit_info[1][itime] )
      (simHitIt->second).hit_info[1][itime] = tof;

  } // ihit loop

}
