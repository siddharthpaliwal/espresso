#include <math.h>
#include <mpi.h>
#include "communication.h"
#include "particle_data.h"
#include "interaction_data.h"
#include "cells.h"
#include "cells.h"
#include "config.h"
#include "elc.h"
#include "mmm-common.h"
#include "utils.h"
#include "pressure.h"
#include "p3m.h"

#ifdef ELECTROSTATICS

//#define CHECKPOINTS
// #define LOG_FORCES

/****************************************
 * LOCAL DEFINES
 ****************************************/

/* Largest reasonable cutoff for far formula */
#define MAXIMAL_FAR_CUT 50

/****************************************
 * LOCAL VARIABLES
 ****************************************/

static double ux, ux2, uy, uy2, uz;

ELC_struct elc_params = { 1e100, 10, 1 };

/****************************************
 * LOCAL ARRAYS
 ****************************************/

/* product decomposition data organization. For the cell blocks
   it is assumed that the lower blocks part is in the lower half.
   This has to have positive sign, so that has to be first. */
#define POQESP 0
#define POQECP 1
#define POQESM 2
#define POQECM 3

#define PQESSP 0
#define PQESCP 1
#define PQECSP 2
#define PQECCP 3
#define PQESSM 4
#define PQESCM 5
#define PQECSM 6
#define PQECCM 7

/* number of local particles */
static int n_localpart = 0;

/* temporary buffers for product decomposition */
static double *partblk = NULL;
/* collected data from the other cells */
static double gblcblk[8];

/* structure for storing of sin and cos values */
typedef struct {
  double s, c;
} SCCache;

/* sin/cos caching */ 
static SCCache *scxcache = NULL;
static int    n_scxcache;  
static SCCache *scycache = NULL;
static int    n_scycache;  

/****************************************
 * LOCAL FUNCTIONS
 ****************************************/

/* sin/cos storage */
static void prepare_scx_cache();
static void prepare_scy_cache();
/* common code */
static void distribute(int size);
/* p=0 per frequency code */
static void setup_P(int p, double omega);
static void add_P_force();
static double   P_energy(double omega);
/* q=0 per frequency code */
static void setup_Q(int q, double omega);
static void add_Q_force();
static double   Q_energy(double omega);
/* p,q <> 0 per frequency code */
static void setup_PQ(int p, int q, double omega);
static void add_PQ_force(int p, int q, double omega);
static double   PQ_energy(double omega);
static void add_dipole_force();
static double dipole_energy();

/* COMMON */
/**********/

void ELC_setup_constants()
{
  ux  = 1/box_l[0];
  ux2 = ux*ux;
  uy  = 1/box_l[1];
  uy2 = uy*uy;  
  uz  = 1/box_l[2];
}

/* SC Cache */
/************/
static void prepare_scx_cache()
{
  int np, c, i, ic, freq, o;
  double pref, arg;
  Particle *part;
  
  for (freq = 1; freq <= n_scxcache; freq++) {
    pref = C_2PI*ux*freq;
    o = (freq-1)*n_localpart;
    ic = 0;
    for (c = 0; c < local_cells.n; c++) {
      np   = local_cells.cell[c]->n;
      part = local_cells.cell[c]->part;
      for (i = 0; i < np; i++) {
	arg = pref*part[i].r.p[0];
	scxcache[o + ic].s = sin(arg);
	scxcache[o + ic].c = cos(arg);
	ic++;
      }
    }
  }
}

static void prepare_scy_cache()
{
  int np, c, i, ic, freq, o;
  double pref, arg;
  Particle *part;
  
  for (freq = 1; freq <= n_scycache; freq++) {
    pref = C_2PI*uy*freq;
    o = (freq-1)*n_localpart;
    ic = 0;
    for (c = 0; c < local_cells.n; c++) {
      np   = local_cells.cell[c]->n;
      part = local_cells.cell[c]->part;
      for (i = 0; i < np; i++) {
	arg = pref*part[i].r.p[1];
	scycache[o + ic].s = sin(arg);
	scycache[o + ic].c = cos(arg);
	ic++;
      }
    }
  }
}

/*****************************************************************/
/* data distribution */
/*****************************************************************/

MDINLINE void clear_vec(double *pdc, int size)
{
  int i;
  for (i = 0; i < size; i++)
    pdc[i] = 0;
}

MDINLINE void copy_vec(double *pdc_d, double *pdc_s, int size)
{
  int i;
  for (i = 0; i < size; i++)
    pdc_d[i] = pdc_s[i];
}

MDINLINE void add_vec(double *pdc_d, double *pdc_s1, double *pdc_s2, int size)
{
  int i;
  for (i = 0; i < size; i++)
    pdc_d[i] = pdc_s1[i] + pdc_s2[i];
}

MDINLINE void scale_vec(double scale, double *pdc, int size)
{
  int i;
  for (i = 0; i < size; i++)
    pdc[i] *= scale;
}

MDINLINE double *block(double *p, int index, int size)
{
  return &p[index*size];
}

void distribute(int size)
{
  double send_buf[8];
  copy_vec(send_buf, gblcblk, size);
  MPI_Allreduce(send_buf, gblcblk, size, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
}

#ifdef CHECKPOINTS
static void checkpoint(char *text, int p, int q, int e_size)
{
  int c, i;
  fprintf(stderr, "%d: %s %d %d\n", this_node, text, p, q);

  fprintf(stderr, "partblk\n");
  for (c = 0; c < n_localpart; c++) {
    fprintf(stderr, "%d", c);    
    for (i = 0; i < e_size; i++)
      fprintf(stderr, " %10.3g", block(partblk, c, 2*e_size)[i]);
    fprintf(stderr, " m");
    for (i = 0; i < e_size; i++)
      fprintf(stderr, " %10.3g", block(partblk, c, 2*e_size)[i + e_size]);
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "\n");

  fprintf(stderr, "gblcblk\n");
  for (i = 0; i < e_size; i++)
    fprintf(stderr, " %10.3g", gblcblk[i]);
  fprintf(stderr, " m");
  for (i = 0; i < e_size; i++)
    fprintf(stderr, " %10.3g", gblcblk[i + e_size]);
  fprintf(stderr, "\n");
}

#else
#define checkpoint(text,p,q,size)
#endif

#ifdef LOG_FORCES
static void clear_log_forces(char *where)
{
  int np, c, i, j;
  Particle *part;

  fprintf(stderr, "%s\n", where);
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      fprintf(stderr, "%d %g %g %g\n", part[i].p.identity,
	      part[i].f.f[0], part[i].f.f[1], part[i].f.f[2]);
      for (j = 0; j < 3; j++)
	part[i].f.f[j] = 0;
    }
  }
}
#else
#define clear_log_forces(w)
#endif

/*****************************************************************/
/* dipole terms */
/*****************************************************************/

static void add_dipole_force()
{
  int np, c, i;
  Particle *part;
  double pref = coulomb.prefactor*4*M_PI*ux*uy*uz;

  gblcblk[0] = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++)
      gblcblk[0] += part[i].p.q*part[i].r.p[2];
  }
  gblcblk[0] *= pref;

  distribute(1);

  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++)
      part[i].f.f[2] -= gblcblk[0]*part[i].p.q;
  }
}

static double dipole_energy()
{
  int np, c, i;
  Particle *part;
  double pref = coulomb.prefactor*2*M_PI*ux*uy*uz;

  gblcblk[0] = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++)
      gblcblk[0] += part[i].p.q*part[i].r.p[2];
  }

  distribute(1);
  
  return pref*SQR(gblcblk[0]);
}

/*****************************************************************/
/* PoQ exp sum */
/*****************************************************************/

static void setup_P(int p, double omega)
{
  int np, c, i, ic, o = (p-1)*n_localpart;
  Particle *part;
  double pref = -coulomb.prefactor*4*M_PI*ux*uy/(exp(omega*box_l[2]) - 1);
  double e;
  int size = 4;

  clear_vec(gblcblk, size);
  ic = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      e = exp(omega*part[i].r.p[2]);

      partblk[size*ic + POQESM] = part[i].p.q*scxcache[o + ic].s/e;
      partblk[size*ic + POQESP] = part[i].p.q*scxcache[o + ic].s*e;
      partblk[size*ic + POQECM] = part[i].p.q*scxcache[o + ic].c/e;
      partblk[size*ic + POQECP] = part[i].p.q*scxcache[o + ic].c*e;

      add_vec(gblcblk, gblcblk, block(partblk, ic, size), size);
      ic++;
    }
  }
  scale_vec(pref, gblcblk, size);
}

static void setup_Q(int q, double omega)
{
  int np, c, i, ic, o = (q-1)*n_localpart;
  Particle *part;
  double pref = -coulomb.prefactor*4*M_PI*ux*uy/(exp(omega*box_l[2]) - 1);
  double e;
  int size = 4;

  clear_vec(gblcblk, size);
  ic = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      e = exp(omega*part[i].r.p[2]);

      partblk[size*ic + POQESM] = part[i].p.q*scycache[o + ic].s/e;
      partblk[size*ic + POQESP] = part[i].p.q*scycache[o + ic].s*e;
      partblk[size*ic + POQECM] = part[i].p.q*scycache[o + ic].c/e;
      partblk[size*ic + POQECP] = part[i].p.q*scycache[o + ic].c*e;

      add_vec(gblcblk, gblcblk, block(partblk, ic, size), size);
      ic++;
    }
  }
  scale_vec(pref, gblcblk, size);
}

static void add_P_force()
{
  int np, c, i, ic;
  Particle *part;
  int size = 4;

  ic = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      part[i].f.f[0] +=
	partblk[size*ic + POQESM]*gblcblk[POQECP] - partblk[size*ic + POQECM]*gblcblk[POQESP] +
	partblk[size*ic + POQESP]*gblcblk[POQECM] - partblk[size*ic + POQECP]*gblcblk[POQESM];
      part[i].f.f[2] +=
	partblk[size*ic + POQECM]*gblcblk[POQECP] + partblk[size*ic + POQESM]*gblcblk[POQESP] -
	partblk[size*ic + POQECP]*gblcblk[POQECM] - partblk[size*ic + POQESP]*gblcblk[POQESM];
      ic++;
    }
  }
}

static double P_energy(double omega)
{
  int np, c, i, ic;
  Particle *part;
  int size = 4;
  double eng = 0;
  double pref = 1/omega;

  ic = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      eng += pref*(partblk[size*ic + POQECM]*gblcblk[POQECP] + partblk[size*ic + POQESM]*gblcblk[POQESP] +
		   partblk[size*ic + POQECP]*gblcblk[POQECM] + partblk[size*ic + POQESP]*gblcblk[POQESM]);
      ic++;
    }
  }
  return eng;
}

static void add_Q_force()
{
  int np, c, i, ic;
  Particle *part;
  int size = 4;

  ic = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      part[i].f.f[1] +=
	partblk[size*ic + POQESM]*gblcblk[POQECP] - partblk[size*ic + POQECM]*gblcblk[POQESP] +
	partblk[size*ic + POQESP]*gblcblk[POQECM] - partblk[size*ic + POQECP]*gblcblk[POQESM];
      part[i].f.f[2] +=
	partblk[size*ic + POQECM]*gblcblk[POQECP] + partblk[size*ic + POQESM]*gblcblk[POQESP] -
	partblk[size*ic + POQECP]*gblcblk[POQECM] - partblk[size*ic + POQESP]*gblcblk[POQESM];
      ic++;
    }
  }
}

static double Q_energy(double omega)
{
  int np, c, i, ic;
  Particle *part;
  int size = 4;
  double eng = 0;
  double pref = 1/omega;

  ic = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      eng += pref*(partblk[size*ic + POQECM]*gblcblk[POQECP] + partblk[size*ic + POQESM]*gblcblk[POQESP] +
		   partblk[size*ic + POQECP]*gblcblk[POQECM] + partblk[size*ic + POQESP]*gblcblk[POQESM]);
      ic++;
    }
  }
  return eng;
}

/*****************************************************************/
/* PQ particle blocks */
/*****************************************************************/

static void setup_PQ(int p, int q, double omega)
{
  int np, c, i, ic, ox = (p - 1)*n_localpart, oy = (q - 1)*n_localpart;
  Particle *part;
  double pref = -coulomb.prefactor*8*M_PI*ux*uy/(exp(omega*box_l[2]) - 1);
  double e;
  int size = 8;

  clear_vec(gblcblk, size);
  ic = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      e = exp(omega*part[i].r.p[2]);
      
      partblk[size*ic + PQESSM] = scxcache[ox + ic].s*scycache[oy + ic].s*part[i].p.q/e;
      partblk[size*ic + PQESCM] = scxcache[ox + ic].s*scycache[oy + ic].c*part[i].p.q/e;
      partblk[size*ic + PQECSM] = scxcache[ox + ic].c*scycache[oy + ic].s*part[i].p.q/e;
      partblk[size*ic + PQECCM] = scxcache[ox + ic].c*scycache[oy + ic].c*part[i].p.q/e;

      partblk[size*ic + PQESSP] = scxcache[ox + ic].s*scycache[oy + ic].s*part[i].p.q*e;
      partblk[size*ic + PQESCP] = scxcache[ox + ic].s*scycache[oy + ic].c*part[i].p.q*e;
      partblk[size*ic + PQECSP] = scxcache[ox + ic].c*scycache[oy + ic].s*part[i].p.q*e;
      partblk[size*ic + PQECCP] = scxcache[ox + ic].c*scycache[oy + ic].c*part[i].p.q*e;

      add_vec(gblcblk, gblcblk, block(partblk, ic, size), size);
      ic++;
    }
  }
  scale_vec(pref, gblcblk, size);
}

static void add_PQ_force(int p, int q, double omega)
{
  int np, c, i, ic;
  Particle *part;
  double pref_x = C_2PI*ux*p/omega;
  double pref_y = C_2PI*uy*q/omega;
  int size = 8;

  ic = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      part[i].f.f[0] +=
	pref_x*(partblk[size*ic + PQESCM]*gblcblk[PQECCP] + partblk[size*ic + PQESSM]*gblcblk[PQECSP] -
		partblk[size*ic + PQECCM]*gblcblk[PQESCP] - partblk[size*ic + PQECSM]*gblcblk[PQESSP] +
		partblk[size*ic + PQESCP]*gblcblk[PQECCM] + partblk[size*ic + PQESSP]*gblcblk[PQECSM] -
		partblk[size*ic + PQECCP]*gblcblk[PQESCM] - partblk[size*ic + PQECSP]*gblcblk[PQESSM]);
      part[i].f.f[1] +=
	pref_y*(partblk[size*ic + PQECSM]*gblcblk[PQECCP] + partblk[size*ic + PQESSM]*gblcblk[PQESCP] -
		partblk[size*ic + PQECCM]*gblcblk[PQECSP] - partblk[size*ic + PQESCM]*gblcblk[PQESSP] +
		partblk[size*ic + PQECSP]*gblcblk[PQECCM] + partblk[size*ic + PQESSP]*gblcblk[PQESCM] -
		partblk[size*ic + PQECCP]*gblcblk[PQECSM] - partblk[size*ic + PQESCP]*gblcblk[PQESSM]);
      part[i].f.f[2] +=
	       (partblk[size*ic + PQECCM]*gblcblk[PQECCP] + partblk[size*ic + PQECSM]*gblcblk[PQECSP] +
	        partblk[size*ic + PQESCM]*gblcblk[PQESCP] + partblk[size*ic + PQESSM]*gblcblk[PQESSP] -
	        partblk[size*ic + PQECCP]*gblcblk[PQECCM] - partblk[size*ic + PQECSP]*gblcblk[PQECSM] -
	        partblk[size*ic + PQESCP]*gblcblk[PQESCM] - partblk[size*ic + PQESSP]*gblcblk[PQESSM]);
      ic++;
    }
  }
}

static double PQ_energy(double omega)
{
  int np, c, i, ic;
  Particle *part;
  int size = 8;
  double eng = 0;
  double pref = 1/omega;

  ic = 0;
  for (c = 0; c < local_cells.n; c++) {
    np   = local_cells.cell[c]->n;
    part = local_cells.cell[c]->part;
    for (i = 0; i < np; i++) {
      eng += pref*(partblk[size*ic + PQECCM]*gblcblk[PQECCP] + partblk[size*ic + PQECSM]*gblcblk[PQECSP] +
		   partblk[size*ic + PQESCM]*gblcblk[PQESCP] + partblk[size*ic + PQESSM]*gblcblk[PQESSP] +
		   partblk[size*ic + PQECCP]*gblcblk[PQECCM] + partblk[size*ic + PQECSP]*gblcblk[PQECSM] +
		   partblk[size*ic + PQESCP]*gblcblk[PQESCM] + partblk[size*ic + PQESSP]*gblcblk[PQESSM]);
      ic++;
    }
  }
  return eng;
}

/*****************************************************************/
/* main loops */
/*****************************************************************/

void ELC_add_force()
{
  int p, q;
  double omega;

  prepare_scx_cache();
  prepare_scy_cache();

  clear_log_forces("start");

  add_dipole_force();

  clear_log_forces("dipole");

  /* the second condition is just for the case of numerical accident */
  for (p = 1; ux*(p - 1) < elc_params.far_cut && p <= n_scxcache; p++) {
    omega = C_2PI*ux*p;
    setup_P(p, omega);
    distribute(4);
    add_P_force();
    checkpoint("************distri p", p, 0, 2);
  }

  for (q = 1; uy*(q - 1) < elc_params.far_cut && q <= n_scycache; q++) {
    omega = C_2PI*uy*q;
    setup_Q(q, omega);
    distribute(4);
    add_Q_force();
    checkpoint("************distri q", 0, q, 2);
  }

  for (p = 1; p - 1 < ux*elc_params.far_cut  && p <= n_scxcache ; p++) {
    for (q = 1; SQR(ux*(p - 1)) + SQR(uy*(q - 1)) < elc_params.far_cut2 && q <= n_scycache; q++) {
      omega = C_2PI*sqrt(SQR(ux*p) + SQR(uy*q));
      setup_PQ(p, q, omega);
      distribute(8);
      add_PQ_force(p, q, omega);
      checkpoint("************distri pq", p, q, 4);
    }
  }

  clear_log_forces("end");
}

double ELC_energy()
{
  double eng;
  int p, q;
  double omega;

  eng = dipole_energy();

  prepare_scx_cache();
  prepare_scy_cache();

  /* the second condition is just for the case of numerical accident */
  for (p = 1; ux*(p - 1) < elc_params.far_cut && p <= n_scxcache; p++) {
    omega = C_2PI*ux*p;
    setup_P(p, omega);
    distribute(4);
    eng += P_energy(omega);
    checkpoint("E************distri p", p, 0, 2);
  }

  for (q = 1; uy*(q - 1) < elc_params.far_cut && q <= n_scycache; q++) {
    omega = C_2PI*uy*q;
    setup_Q(q, omega);
    distribute(4);
    eng += Q_energy(omega);
    checkpoint("E************distri q", 0, q, 2);
  }

  for (p = 1; p - 1 < ux*elc_params.far_cut  && p <= n_scxcache ; p++) {
    for (q = 1; SQR(ux*(p - 1)) + SQR(uy*(q - 1)) < elc_params.far_cut2 && q <= n_scycache; q++) {
      omega = C_2PI*sqrt(SQR(ux*p) + SQR(uy*q));
      setup_PQ(p, q, omega);
      distribute(8);
      eng += PQ_energy(omega);
      checkpoint("E************distri pq", p, q, 4);
    }
  }

  /* we count both i<->j and j<->i, so return just half of it */
  return 0.5*eng;
}

static char *ELC_tune(double error)
{
  double err, step;
  double h = box_l[2] - elc_params.minimal_dist,
    dst1 = elc_params.minimal_dist,
    dst2 = 2*box_l[2] - elc_params.minimal_dist;
  elc_params.far_cut = 2;
  /* so it is at least at one border optimal */
  step = dmin(ux, uy);
  do {
    err = 0.5*(exp(2*M_PI*elc_params.far_cut*h)/dst1*
	       (C_2PI*elc_params.far_cut + 2*(ux + uy) + 1/dst1)/
	       (exp(2*M_PI*elc_params.far_cut*box_l[2])- 1) +
	       exp(-2*M_PI*elc_params.far_cut*h)/dst2*
	       (C_2PI*elc_params.far_cut + 2*(ux + uy) + 1/dst2)/
	       (exp(2*M_PI*elc_params.far_cut*box_l[2])- 1));
    elc_params.far_cut += step;
  }
  while (err > error && elc_params.far_cut < MAXIMAL_FAR_CUT);
  if (elc_params.far_cut >= MAXIMAL_FAR_CUT)
    return "Far cutoff too large, increase gap size";
  // fprintf(stderr, "far cutoff %g %g\n", elc_params.far_cut, err);
  elc_params.far_cut -= step;
  elc_params.far_cut2 = SQR(elc_params.far_cut);
  return NULL;
}

/****************************************
 * COMMON PARTS
 ****************************************/

void ELC_init()
{
  char *err;

  if (!PERIODIC(0) || !PERIODIC(1) || !PERIODIC(2)) {
    fprintf(stderr, "ERROR: ELC requires periodicity 1 1 1\n");
    errexit();
  }

  ELC_setup_constants();
  if (elc_params.far_calculated) {
    if ((err = ELC_tune(elc_params.maxPWerror))) {
      fprintf(stderr, "ERROR: ELC auto-retuning: %s\n", err);
      errexit();
    }
  }
}

void ELC_on_resort_particles()
{
  n_localpart = cells_get_n_particles();
  n_scxcache = (int)(ceil(elc_params.far_cut/ux) + 1);
  n_scycache = (int)(ceil(elc_params.far_cut/uy) + 1);
  scxcache = realloc(scxcache, n_scxcache*n_localpart*sizeof(SCCache));
  scycache = realloc(scycache, n_scycache*n_localpart*sizeof(SCCache));
    
  partblk   = realloc(partblk,  n_localpart*8*sizeof(double));
}

int set_elc_params(Tcl_Interp *interp, double maxPWerror, double minimal_dist, double far_cut)
{
  char *err;

  elc_params.maxPWerror = maxPWerror;
  elc_params.minimal_dist = minimal_dist;

  ELC_setup_constants();

  switch (coulomb.method) {
  case COULOMB_P3M:
    if (p3m.epsilon != P3M_EPSILON_METALLIC) {
      fprintf(stderr, "WARNING: Forcing P3M to metallic boundary conditions for ELC\n");
      p3m.epsilon = P3M_EPSILON_METALLIC;
    }
    break;
  case COULOMB_NONE:
    Tcl_AppendResult(interp, "please choose 3d electrostatics method before enabling ELC", (char *)NULL);
    return TCL_ERROR;
  default:
    Tcl_AppendResult(interp, "electrostatic method not supported with ELC\n ", (char *)NULL);
    return TCL_ERROR;
  }

  if (far_cut >= 0) {
    elc_params.far_cut = far_cut;
    elc_params.far_cut2 = SQR(far_cut);
    elc_params.far_calculated = 0;
  }
  else {
    if ((err = ELC_tune(maxPWerror))) {
      Tcl_AppendResult(interp, err, NULL);
      return TCL_ERROR;
    }
    elc_params.far_calculated = 1;    
  }

  coulomb.use_elc = 1;

  mpi_bcast_coulomb_params();

  return TCL_OK;
}

#endif
