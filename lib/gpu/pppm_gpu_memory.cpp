/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Charge/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */
 
/* ----------------------------------------------------------------------
   Contributing authors: Mike Brown (ORNL), brownw@ornl.gov
------------------------------------------------------------------------- */

#ifdef USE_OPENCL
#include "pppm_gpu_cl.h"
#define MEM_THREADS 16
#else
#include "pppm_gpu_ptx.h"
#endif
#include "pppm_gpu_memory.h"
#include <cassert>

// Maximum order for stencil
#define MAX_STENCIL 8
// Thread block size for all kernels (Must be >=MAX_STENCIL^2)
#define BLOCK_1D 64
// Number of threads per pencil for charge spread
//#define PENCIL_SIZE MEM_THREADS
#define PENCIL_SIZE 32
// Number of pencils per block for charge spread
//#define BLOCK_PENCILS (BLOCK_1D/PENCIL_SIZE)
#define BLOCK_PENCILS 2

#define PPPMGPUMemoryT PPPMGPUMemory<numtyp, acctyp>

extern PairGPUDevice<PRECISION,ACC_PRECISION> pair_gpu_device;

template <class numtyp, class acctyp>
PPPMGPUMemoryT::PPPMGPUMemory() : _allocated(false), _compiled(false),
                                  _max_bytes(0) {
  device=&pair_gpu_device;
  ans=new PairGPUAns<numtyp,acctyp>();
}

template <class numtyp, class acctyp>
PPPMGPUMemoryT::~PPPMGPUMemory() {
  clear();
  delete ans;
}

template <class numtyp, class acctyp>
int PPPMGPUMemoryT::bytes_per_atom() const {
  return device->atom.bytes_per_atom()+ans->bytes_per_atom()+1;
}

template <class numtyp, class acctyp>
numtyp * PPPMGPUMemoryT::init(const int nlocal, const int nall, FILE *_screen,
                              const int order, const int nxlo_out,
                              const int nylo_out, const int nzlo_out,
                              const int nxhi_out, const int nyhi_out,
                              const int nzhi_out, double **rho_coeff,
                              numtyp **vd_brick, bool &success) {
  clear();
  
  _max_bytes=10;
  screen=_screen;

  if (!device->init(*ans,nlocal,nall)) {
    success=false;
    return 0;
  }
  ucl_device=device->gpu;
  atom=&device->atom;

  _block_size=BLOCK_1D;
  assert(BLOCK_PENCILS*PENCIL_SIZE==BLOCK_1D);
  assert(MAX_STENCIL*MAX_STENCIL<=BLOCK_1D);
  if (static_cast<size_t>(_block_size)>ucl_device->group_size())
    _block_size=ucl_device->group_size();
  compile_kernels(*ucl_device);

  // Initialize timers for the selected GPU
  time_in.init(*ucl_device);
  time_in.zero();
  time_out.init(*ucl_device);
  time_out.zero();
  time_map.init(*ucl_device);
  time_map.zero();
  time_rho.init(*ucl_device);
  time_rho.zero();
  time_interp.init(*ucl_device);
  time_interp.zero();

  pos_tex.bind_float(atom->dev_x,4);
  q_tex.bind_float(atom->dev_q,1);

  _allocated=true;
  _max_bytes=0;
  _max_an_bytes=ans->gpu_bytes();
  
  _order=order;
  _order_m_1=order-1;
  _order2=_order_m_1*_order;
  _nlower=-(_order-1)/2;
  _nupper=order/2;
  _nxlo_out=nxlo_out;
  _nylo_out=nylo_out;
  _nzlo_out=nzlo_out;
  _nxhi_out=nxhi_out;
  _nyhi_out=nyhi_out;
  _nzhi_out=nzhi_out;
  _max_brick_atoms=10;

  // Get rho_coeff on device
  int n2lo=(1-order)/2;
  int numel=order*( order/2 - n2lo + 1 );
  success=success && (d_rho_coeff.alloc(numel,*ucl_device,UCL_READ_ONLY)==
                      UCL_SUCCESS);
  UCL_H_Vec<double> view;
  view.view(rho_coeff[0]+n2lo,numel,*ucl_device);
  ucl_copy(d_rho_coeff,view,true);
  _max_bytes+=d_rho_coeff.row_bytes();
  
  // Allocate storage for grid
  _npts_x=nxhi_out-nxlo_out+1;
  _npts_y=nyhi_out-nylo_out+1;
  _npts_z=nzhi_out-nzlo_out+1;
  _npts_yx=_npts_x*_npts_y;
  success=success && (d_brick.alloc(_npts_x*_npts_y*_npts_z*4,*ucl_device)==
                      UCL_SUCCESS);
  success=success && (h_brick.alloc(_npts_x*_npts_y*_npts_z,*ucl_device)==
                      UCL_SUCCESS);
  success=success && (h_vd_brick.alloc(_npts_x*_npts_y*_npts_z*4,*ucl_device)==
                      UCL_SUCCESS);
  *vd_brick=h_vd_brick.begin();
  _max_bytes+=d_brick.row_bytes();

  // Allocate vector with count of atoms assigned to each grid point
  _nlocal_x=_npts_x+_nlower-_nupper;
  _nlocal_y=_npts_y+_nlower-_nupper;
  _nlocal_z=_npts_z+_nlower-_nupper;
  _nlocal_yx=_nlocal_x*_nlocal_y;
  _atom_stride=_nlocal_x*_nlocal_y*_nlocal_z;
  success=success && (d_brick_counts.alloc(_atom_stride,*ucl_device)==
                      UCL_SUCCESS);
  _max_bytes+=d_brick_counts.row_bytes();

  // Allocate storage for atoms assigned to each grid point
  success=success && (d_brick_atoms.alloc(_atom_stride*_max_brick_atoms,
                                          *ucl_device)==UCL_SUCCESS);
  _max_bytes+=d_brick_atoms.row_bytes();

  // Allocate error flags for checking out of bounds atoms
  success=success && (h_error_flag.alloc(1,*ucl_device)==UCL_SUCCESS);
  success=success && (d_error_flag.alloc(1,*ucl_device,UCL_WRITE_ONLY)==
                                         UCL_SUCCESS);
  d_error_flag.zero();
  _max_bytes+=1;

//success=success && (force_temp.alloc(nall*4*2,*ucl_device)==UCL_SUCCESS);
  
  return h_brick.begin();
}

template <class numtyp, class acctyp>
void PPPMGPUMemoryT::clear() {
  if (!_allocated)
    return;
  _allocated=false;
  
  d_brick.clear();
  h_brick.clear();
  h_vd_brick.clear();
  d_brick_counts.clear();
  h_error_flag.clear();
  d_error_flag.clear();
  d_brick_atoms.clear();
  
  acc_timers();
  device->output_kspace_times(time_in,time_out,time_map,time_rho,time_interp,
                              *ans,_max_bytes+_max_an_bytes,screen);

  if (_compiled) {
    k_particle_map.clear();
    k_make_rho.clear();
    k_interp.clear();
    delete pppm_program;
    _compiled=false;
  }

  time_in.clear();
  time_out.clear();
  time_map.clear();
  time_rho.clear();
  time_interp.clear();

  device->clear();
}

// ---------------------------------------------------------------------------
// Charge spreading stuff
// ---------------------------------------------------------------------------
template <class numtyp, class acctyp>
int PPPMGPUMemoryT::spread(const int ago, const int nlocal, const int nall,
                           double **host_x, int *host_type, bool &success,
                           double *host_q, double *boxlo, 
                           const double delxinv, const double delyinv,
                           const double delzinv) {
  acc_timers();
  if (nlocal==0) {
    zero_timers();
    return 0;
  }
  
  ans->inum(nlocal);

  if (ago==0) {
    resize_atom(nlocal,nall,success);
    resize_local(nlocal,success);
    if (!success)
      return 0;

    double bytes=ans->gpu_bytes();
    if (bytes>_max_an_bytes)
      _max_an_bytes=bytes;
  }

  atom->cast_x_data(host_x,host_type);
  atom->cast_q_data(host_q);
  atom->add_x_data(host_x,host_type);
  atom->add_q_data();

  time_map.start();

  // Compute the block size and grid size to keep all cores busy
  int BX=this->block_size();
  int GX=static_cast<int>(ceil(static_cast<double>(this->ans->inum())/BX));

  int _max_atoms=10;
  int ainum=this->ans->inum();
  
  // Boxlo adjusted to be upper left brick and shift for even stencil order
  double shift=0.0;
  if (_order % 2)
    shift=0.5;
  _brick_x=boxlo[0]+(_nxlo_out-_nlower-shift)/delxinv;
  _brick_y=boxlo[1]+(_nylo_out-_nlower-shift)/delyinv;
  _brick_z=boxlo[2]+(_nzlo_out-_nlower-shift)/delzinv;
  
  _delxinv=delxinv;
  _delyinv=delyinv;
  _delzinv=delzinv;
  double delvolinv = delxinv*delyinv*delzinv;
  numtyp f_delvolinv = delvolinv;

  d_brick_counts.zero();
  k_particle_map.set_size(GX,BX);
  k_particle_map.run(&atom->dev_x.begin(), &atom->dev_q.begin(), &f_delvolinv,
                     &ainum, &d_brick_counts.begin(), &d_brick_atoms.begin(),
                     &_brick_x, &_brick_y, &_brick_z, &_delxinv, &_delyinv, 
                     &_delzinv, &_nlocal_x, &_nlocal_y, &_nlocal_z, 
                     &_atom_stride, &_max_brick_atoms, &d_error_flag.begin());
  time_map.stop();

  time_rho.start();

  BX=block_size();
  GX=static_cast<int>(ceil(static_cast<double>(_npts_y*_npts_z)/BLOCK_PENCILS));
  k_make_rho.set_size(GX,BX);
  k_make_rho.run(&atom->dev_x.begin(), &atom->dev_q.begin(),
                 &d_brick_counts.begin(), &d_brick_atoms.begin(),
                 &d_brick.begin(), &d_rho_coeff.begin(), &_atom_stride, &_npts_x,
                 &_npts_y, &_npts_z, &_nlocal_x, &_nlocal_y, &_nlocal_z,
                 &_order_m_1, &_order, &_order2);
  time_rho.stop();

  time_out.start();
  ucl_copy(h_brick,d_brick,_npts_yx*_npts_z,true);
  ucl_copy(h_error_flag,d_error_flag,false);
  time_out.stop();
  
  if (h_error_flag[0]==2) {
    // Not enough storage for atoms on the brick
    _max_brick_atoms*=2;
    d_error_flag.zero();
    d_brick_atoms.clear();
    d_brick_atoms.alloc(_atom_stride*_max_atoms,*ucl_device);
    _max_bytes+=d_brick_atoms.row_bytes();
    return spread(ago,nlocal,nall,host_x,host_type,success,host_q,boxlo, 
                  delxinv,delyinv,delzinv);
  }
  
  return h_error_flag[0];
}

// ---------------------------------------------------------------------------
// Charge spreading stuff
// ---------------------------------------------------------------------------
template <class numtyp, class acctyp>
void PPPMGPUMemoryT::interp(const numtyp qqrd2e_scale) {
  time_in.start();
  ucl_copy(d_brick,h_vd_brick,true);
  time_in.stop();
  
  time_interp.start();
  // Compute the block size and grid size to keep all cores busy
  int BX=this->block_size();
  int GX=static_cast<int>(ceil(static_cast<double>(this->ans->inum())/BX));

  int ainum=this->ans->inum();
  
  k_interp.set_size(GX,BX);
  k_interp.run(&atom->dev_x.begin(), &atom->dev_q.begin(), &ainum, 
               &d_brick.begin(), &d_rho_coeff.begin(), &_npts_x, &_npts_yx,
               &_brick_x, &_brick_y, &_brick_z, &_delxinv, &_delyinv, &_delzinv,
               &_order, &_order2, &qqrd2e_scale, &ans->dev_ans.begin());
  time_interp.stop();

//ucl_copy(force_temp,ans->dev_ans,ans->dev_ans.numel(),false);
}


template <class numtyp, class acctyp>
double PPPMGPUMemoryT::host_memory_usage() const {
  return device->atom.host_memory_usage()+sizeof(PPPMGPUMemory<numtyp,acctyp>);
}

template <class numtyp, class acctyp>
void PPPMGPUMemoryT::compile_kernels(UCL_Device &dev) {
  if (_compiled)
    return;

  std::string flags="-cl-fast-relaxed-math -cl-mad-enable "+
                    std::string(OCL_PRECISION_COMPILE);

  pppm_program=new UCL_Program(dev);
  pppm_program->load_string(pppm_gpu_kernel,flags.c_str());
  k_particle_map.set_function(*pppm_program,"particle_map");
  k_make_rho.set_function(*pppm_program,"make_rho");
  k_interp.set_function(*pppm_program,"interp");
  pos_tex.get_texture(*pppm_program,"pos_tex");
  q_tex.get_texture(*pppm_program,"q_tex");

  _compiled=true;
}

template class PPPMGPUMemory<PRECISION,ACC_PRECISION>;

