/* Copyright 2008-2010 ENSEIRB, INRIA & CNRS
**
** This file is part of the Scotch software package for static mapping,
** graph partitioning and sparse matrix ordering.
**
** This software is governed by the CeCILL-C license under French law
** and abiding by the rules of distribution of free software. You can
** use, modify and/or redistribute the software under the terms of the
** CeCILL-C license as circulated by CEA, CNRS and INRIA at the following
** URL: "http://www.cecill.info".
** 
** As a counterpart to the access to the source code and rights to copy,
** modify and redistribute granted by the license, users are provided
** only with a limited warranty and the software's author, the holder of
** the economic rights, and the successive licensors have only limited
** liability.
** 
** In this respect, the user's attention is drawn to the risks associated
** with loading, using, modifying and/or developing or reproducing the
** software by the user in light of its specific status of free software,
** that may mean that it is complicated to manipulate, and that also
** therefore means that it is reserved for developers and experienced
** professionals having in-depth computer knowledge. Users are therefore
** encouraged to load and test the software's suitability as regards
** their requirements in conditions enabling the security of their
** systems and/or data to be ensured and, more generally, to use and
** operate it in the same conditions as regards security.
** 
** The fact that you are presently reading this means that you have had
** knowledge of the CeCILL-C license and that you accept its terms.
*/
/************************************************************/
/**                                                        **/
/**   NAME       : kdgraph_map_rb_part.c                   **/
/**                                                        **/
/**   AUTHOR     : Francois PELLEGRINI                     **/
/**                                                        **/
/**   FUNCTION   : This module performs the Dual Recursive **/
/**                Bipartitioning mapping algorithm        **/ 
/**                in parallel. It does so for complete    **/
/**                graph architectures, hence performing   **/
/**                plain graph partitioning, which         **/
/**                avoids to take care of what the other   **/
/**                processes are doing.                    **/
/**                                                        **/
/**   DATES      : # Version 5.1  : from : 21 jun 2008     **/
/**                                 to     30 jul 2010     **/
/**                                                        **/
/************************************************************/

/*
**  The defines and includes.
*/

#define KDGRAPH_MAP_RB

#include "module.h"
#include "common.h"
#include "parser.h"
#include "graph.h"
#include "arch.h"
#include "bgraph.h"
#include "bgraph_bipart_st.h"
#include "mapping.h"
#include "kgraph.h"
#include "kgraph_map_st.h"
#include "dgraph.h"
#include "dmapping.h"
#include "bdgraph.h"
#include "bdgraph_bipart_st.h"
#include "kdgraph.h"
#include "kdgraph_map_rb.h"
#include "kdgraph_map_rb_part.h"
#include "kdgraph_map_st.h"

/*****************************/
/*                           */
/* This is the main routine. */
/*                           */
/*****************************/

/* This routine sequentially computes a mapping
** of the given subgraph and adds its result to
** the given distributed mapping. Since no
** cocycle data is needed, the un-synchronized
** sequential Scotch routine can be used as is.
** It returns:
** - 0   : if the mapping could be computed.
** - !0  : on error.
*/


static
int
kdgraphMapRbPartSequ (
KdgraphMapRbPartGraph * restrict const  grafptr,
Dmapping * restrict const               mappptr,
const Strat * restrict const            stratptr)
{
  Graph * restrict          cgrfptr;
  Kgraph                    kgrfdat;              /* Centralized mapping graph */
  DmappingFrag * restrict   fragptr;

  cgrfptr = &grafptr->data.cgrfdat;
  if (mapInit2 (&kgrfdat.m, cgrfptr->baseval, cgrfptr->vertnbr, &mappptr->archdat, &grafptr->domnorg) != 0) {
    errorPrint ("kdgraphMapRbPartSequ: cannot initialize centralized mapping");
    return     (1);
  }

  if (kgraphInit (&kgrfdat, cgrfptr, &kgrfdat.m) != 0) {
    errorPrint ("kdgraphMapRbPartSequ: cannot initialize centralized graph");
    return     (1);
  }
  kgrfdat.s.flagval = cgrfptr->flagval;           /* Free sequential graph along with mapping data */
  kgrfdat.s.vnumtax = NULL;                       /* Remove index array if any                     */

  if (kgraphMapSt (&kgrfdat, stratptr) != 0) {    /* Compute sequential mapping */
    kgraphExit (&kgrfdat);
    return     (1);
  }

  if (((fragptr = memAlloc (sizeof (DmappingFrag))) == NULL) ||
      ((fragptr->vnumtab = memAlloc (cgrfptr->vertnbr * sizeof (Gnum))) == NULL)) {
    errorPrint ("kdgraphMapRbPartSequ: out of memory");
    if (fragptr != NULL)
      memFree (fragptr);
    kgraphExit (&kgrfdat);
    return     (1);
  }

  fragptr->vertnbr = cgrfptr->vertnbr;
  fragptr->parttab = kgrfdat.m.parttax + kgrfdat.m.baseval;
  fragptr->domnnbr = kgrfdat.m.domnnbr;
  fragptr->domntab = kgrfdat.m.domntab;
  kgrfdat.m.parttax = NULL;                       /* Keep sequential mapping arrays for distributed mapping fragment */
  kgrfdat.m.domntab = NULL;
  if (kgrfdat.m.domnmax > kgrfdat.m.domnnbr)
    fragptr->domntab = memRealloc (fragptr->domntab, kgrfdat.m.domnnbr * sizeof (ArchDom)); /* Reallocate mapping array */

  if (cgrfptr->vnumtax != NULL)
    memCpy (fragptr->vnumtab, cgrfptr->vnumtax + cgrfptr->baseval, cgrfptr->vertnbr * sizeof (Gnum));
  else {
    Gnum                vertadj;
    Gnum                vertnum;

    for (vertnum = 0, vertadj = cgrfptr->baseval; vertnum < cgrfptr->vertnbr; vertnum ++)
      fragptr->vnumtab[vertnum] = vertadj + vertnum;
  }

  dmapAdd (mappptr, fragptr);                     /* Add mapping fragment */

  kgraphExit (&kgrfdat);                          /* Free mapping without some of its arrays */

  return (0);
}

/* This routine builds either a centralized or a
** distributed subgraph, according to the number
** of processes in the given part. The calling
** conventions of this routine have been designed
** so as to allow for multi-threading.
*/

static
void *
kdgraphMapRbPartFold2 (
void * const                    dataptr)          /* Pointer to thread data */
{
  KdgraphMapRbPartData *            fldthrdptr;   /* Thread input parameters      */
  KdgraphMapRbPartGraph * restrict  fldgrafptr;   /* Pointer to folded graph area */
  Dgraph                            indgrafdat;   /* Induced distributed graph    */
  void *                            o;

  fldthrdptr = (KdgraphMapRbPartData *) dataptr;
  fldgrafptr = fldthrdptr->fldgrafptr;

  if (fldthrdptr->fldprocnbr == 0)                /* If recursion stopped, build mapping of graph part */
    return ((void *) (intptr_t) kdgraphMapRbAddPart (fldthrdptr->orggrafptr, fldthrdptr->mappptr, fldthrdptr->inddomnptr, fldthrdptr->indvertnbr,
                                                     fldthrdptr->indparttax + fldthrdptr->orggrafptr->baseval, fldthrdptr->indpartval));

  if (dgraphInducePart (fldthrdptr->orggrafptr, fldthrdptr->indparttax, /* Compute unfinished induced subgraph on all processes */
                        fldthrdptr->indvertnbr, fldthrdptr->indpartval, &indgrafdat) != 0)
    return ((void *) 1);

  if (fldthrdptr->fldprocnbr > 1) {               /* If subpart has several processes, fold a distributed graph                     */
    o = (void *) (intptr_t) dgraphFold2 (&indgrafdat, fldthrdptr->fldpartval, /* Fold temporary induced subgraph from all processes */
                                         &fldgrafptr->data.dgrfdat, fldthrdptr->fldproccomm, NULL, NULL, MPI_INT);
  }
  else {                                          /* Create a centralized graph */
    Graph * restrict      fldcgrfptr;

    fldcgrfptr = (fldthrdptr->fldprocnum == 0) ? &fldgrafptr->data.cgrfdat : NULL; /* See if we are the receiver            */
    o = (void *) (intptr_t) dgraphGather (&indgrafdat, fldcgrfptr); /* Gather centralized subgraph from all other processes */
  }
  dgraphExit (&indgrafdat);                       /* Free temporary induced graph */

  return (o);
}

static
int
kdgraphMapRbPartFold (
Bdgraph * restrict const                actgrafptr,
Dmapping * restrict const               mappptr,
const ArchDom * restrict const          domsubtab,
KdgraphMapRbPartGraph * restrict const  fldgrafptr)
{
  KdgraphMapRbPartData  fldthrdtab[2];
  int                   fldprocnbr;               /* Number of processes in part of this process  */
  int                   fldprocnbr0;              /* Number of processes in first part            */
  int                   fldprocnum;
  int                   fldproccol;
  int                   fldpartval;
  Gnum                  indvertlocmax;            /* Local number of vertices in biggest subgraph */
  int                   indconttab[2];            /* Array of subjob continuation flags           */
  int                   indpartmax;               /* Induced part having most vertices            */
#ifdef SCOTCH_PTHREAD
  Dgraph                orggrafdat;               /* Structure for copying graph fields except communicator */
  pthread_t             thrdval;                  /* Data of second thread                                  */
#endif /* SCOTCH_PTHREAD */
  int                       o;

  indconttab[0] =                                 /* Assume both jobs will not continue */
  indconttab[1] = 0;
  if ((actgrafptr->compglbsize0 != 0) &&           /* If graph has been bipartitioned */
      (actgrafptr->compglbsize0 != actgrafptr->s.vertglbnbr)) {
    if (archVar (&mappptr->archdat)) {            /* If architecture is variable-sized    */
      if (actgrafptr->compglbsize0 > 1)           /* If graph is not single vertex, go on */
        indconttab[0] = 1;
      if ((actgrafptr->s.vertglbnbr - actgrafptr->compglbsize0) > 1)
        indconttab[1] = 1;
    }
    else {                                        /* Architecture is not variable-sized         */
      if (archDomSize (&mappptr->archdat, &domsubtab[0]) > 1) /* Stop when target is one vertex */
        indconttab[0] = 1;
      if (archDomSize (&mappptr->archdat, &domsubtab[1]) > 1)
        indconttab[1] = 1;
    }
  }

  if ((indconttab[0] + indconttab[1]) == 0) {     /* If both subjobs stop    */
    fldgrafptr->procnbr = 0;                      /* Nothing to do on return */
    return (kdgraphMapRbAddBoth (&actgrafptr->s, mappptr, domsubtab, actgrafptr->partgsttax + actgrafptr->s.baseval)); /* Map both subdomains in the same time */
  }

  if ((2 * actgrafptr->compglbsize0) >= actgrafptr->s.vertglbnbr) { /* Get part of largest subgraph */
    indpartmax    = 0;
    indvertlocmax = actgrafptr->complocsize0;
  }
  else {
    indpartmax    = 1;
    indvertlocmax = actgrafptr->s.vertlocnbr - actgrafptr->complocsize0;
  }
  fldprocnbr0 = (actgrafptr->s.procglbnbr + 1) / 2;  /* Get number of processes in part 0 (always more than in part 1) */

  fldthrdtab[0].mappptr     = mappptr;            /* Load data to pass to the subgraph building routines */
  fldthrdtab[0].orggrafptr  = &actgrafptr->s;
  fldthrdtab[0].inddomnptr  = &domsubtab[indpartmax];
  fldthrdtab[0].indvertnbr  = indvertlocmax;
  fldthrdtab[0].indpartval  = indpartmax;
  fldthrdtab[0].indparttax  = actgrafptr->partgsttax;
  fldthrdtab[0].fldgrafptr  = fldgrafptr;
  fldthrdtab[0].fldpartval  = 0;
  fldthrdtab[0].fldprocnbr  = indconttab[indpartmax] * fldprocnbr0; /* Stop if domain limited to one vertex */
  fldthrdtab[1].mappptr     = mappptr;
  fldthrdtab[1].orggrafptr  = &actgrafptr->s;     /* Assume jobs won't be run concurrently */
  fldthrdtab[1].inddomnptr  = &domsubtab[indpartmax ^ 1];
  fldthrdtab[1].indvertnbr  = actgrafptr->s.vertlocnbr - indvertlocmax;
  fldthrdtab[1].indpartval  = indpartmax ^ 1;
  fldthrdtab[1].indparttax  = actgrafptr->partgsttax;
  fldthrdtab[1].fldgrafptr  = fldgrafptr;
  fldthrdtab[1].fldpartval  = 1;
  fldthrdtab[1].fldprocnbr  = indconttab[indpartmax ^ 1] * (actgrafptr->s.procglbnbr - fldprocnbr0); /* Stop if domain limited to one vertex */

  if (actgrafptr->s.proclocnum < fldprocnbr0) {   /* Compute color and rank in our future subpart */
    fldpartval = 0;
    fldprocnum = actgrafptr->s.proclocnum;
    fldprocnbr = fldprocnbr0;
  }
  else {
    fldpartval = 1;
    fldprocnum = actgrafptr->s.proclocnum - fldprocnbr0;
    fldprocnbr = actgrafptr->s.procglbnbr - fldprocnbr0;
  }

  fldgrafptr->domnorg = *fldthrdtab[fldpartval].inddomnptr; /* Set data of our folded graph */
  fldgrafptr->procnbr = fldthrdtab[fldpartval].fldprocnbr;
  fldgrafptr->levlnum = actgrafptr->levlnum + 1;  /* One level down in the DRB process                     */
  fldproccol = fldpartval;                        /* Split color is the part value                         */
  if (fldgrafptr->procnbr <= 1)                   /* If our part will have only one processor or will stop */
    fldproccol = MPI_UNDEFINED;                   /* Do not create any sub-communicator for it             */
  if (MPI_Comm_split (actgrafptr->s.proccomm, fldproccol, fldprocnum, &fldthrdtab[fldpartval].fldproccomm) != MPI_SUCCESS) { /* Assign folded communicator to proper part */
    errorPrint  ("kdgraphMapRbPartFold: communication error");
    return      (1);
  }
  fldthrdtab[fldpartval].fldprocnum      = fldprocnum; /* This will be our rank afterwards  */
  fldthrdtab[fldpartval ^ 1].fldprocnum  = -1;    /* Other part will not be in communicator */
  fldthrdtab[fldpartval ^ 1].fldproccomm = MPI_COMM_NULL;

#ifdef SCOTCH_PTHREAD
  if ((indconttab[0] + indconttab[1]) == 2) {     /* If both subjobs have meaningful things to do in parallel     */
    orggrafdat = actgrafptr->s;                   /* Create a separate graph structure to change its communicator */
    orggrafdat.flagval = (orggrafdat.flagval & ~DGRAPHFREEALL) | DGRAPHFREECOMM;
    fldthrdtab[1].orggrafptr = &orggrafdat;
    MPI_Comm_dup (actgrafptr->s.proccomm, &orggrafdat.proccomm); /* Duplicate communicator to avoid interferences in communications */

    if (pthread_create (&thrdval, NULL, kdgraphMapRbPartFold2, (void *) &fldthrdtab[1]) != 0) /* If could not create thread */
      o = ((int) (intptr_t) kdgraphMapRbPartFold2 ((void *) &fldthrdtab[0])) || /* Perform inductions in sequence           */
          ((int) (intptr_t) kdgraphMapRbPartFold2 ((void *) &fldthrdtab[1]));
    else {                                        /* Newly created thread is processing subgraph 1, so let's process subgraph 0 */
      void *                    o2;

      o = (int) (intptr_t) kdgraphMapRbPartFold2 ((void *) &fldthrdtab[0]); /* Work on copy with private communicator */

      pthread_join (thrdval, &o2);
      o |= (int) (intptr_t) o2;
    }
    MPI_Comm_free (&orggrafdat.proccomm);
  }
  else
#endif /* SCOTCH_PTHREAD */
    o = ((int) (intptr_t) kdgraphMapRbPartFold2 ((void *) &fldthrdtab[0])) || /* Perform inductions in sequence */
        ((int) (intptr_t) kdgraphMapRbPartFold2 ((void *) &fldthrdtab[1]));

  return (o);
}

/* This routine performs the Dual Recursive 
** Bipartitioning mapping in parallel.
** It returns:
** - 0   : if the mapping could be computed.
** - !0  : on error.
*/

static
int 
kdgraphMapRbPart2 (
KdgraphMapRbPartGraph * restrict const   grafptr,
Dmapping * restrict const                mappptr,
const KdgraphMapRbParam * restrict const paraptr)
{
  ArchDom               domsubtab[2];             /* Temporary subdomains        */
  Bdgraph               actgrafdat;               /* Active bipartitioning graph */
  KdgraphMapRbPartGraph indgrafdat;               /* Induced folded graph area   */
  int                   o;

  if (archDomBipart (&mappptr->archdat, &grafptr->domnorg, &domsubtab[0], &domsubtab[1]) != 0) {
    errorPrint ("kdgraphMapRbPart2: cannot bipartition domain");
    return     (1);
  }

  if (dgraphGhst (&grafptr->data.dgrfdat) != 0) { /* Compute ghost edge array if not already present, to have vertgstnbr (and procsidtab) */
    errorPrint ("kdgraphMapRbPart2: cannot compute ghost edge array");
    return     (1);
  }
  
  o = bdgraphInit (&actgrafdat, &grafptr->data.dgrfdat, NULL, &mappptr->archdat, domsubtab); /* Create active graph */
  actgrafdat.levlnum = grafptr->levlnum;          /* Initial level of bipartition graph is DRB recursion level      */
  if ((o != 0) || (bdgraphBipartSt (&actgrafdat, paraptr->stratsep) != 0)) { /* Bipartition edge-separation graph   */
    bdgraphExit (&actgrafdat);
    return      (1);
  }

  o = kdgraphMapRbPartFold (&actgrafdat, mappptr, domsubtab, &indgrafdat);

  bdgraphExit (&actgrafdat);                      /* Free additional bipartitioning data   */
  dgraphExit  (&grafptr->data.dgrfdat);           /* Free graph before going to next level */

  if (o == 0) {
    if (indgrafdat.procnbr == 1)                  /* If sequential job */
      o = kdgraphMapRbPartSequ (&indgrafdat, mappptr, paraptr->stratseq);
    else if (indgrafdat.procnbr > 1)              /* If distributed job */
      o = kdgraphMapRbPart2 (&indgrafdat, mappptr, paraptr);
  }
  return (o);
}

int 
kdgraphMapRbPart (
Kdgraph * restrict const                 grafptr,
Kdmapping * restrict const               mappptr,
const KdgraphMapRbParam * restrict const paraptr)
{
  KdgraphMapRbPartGraph   grafdat;

  if (archDomSize (&mappptr->mappptr->archdat, &grafptr->m.domnorg) <= 1) /* If only one terminal domain in the target   */
    return (kdgraphMapRbAddOne (&grafptr->s, mappptr->mappptr, &grafptr->m.domnorg)); /* Map all vertices to that domain */

  grafdat.domnorg = grafptr->m.domnorg;
  grafdat.procnbr = grafptr->s.procglbnbr;
  grafdat.levlnum = 0;                            /* Set initial DRB level to zero */

  if (grafptr->s.procglbnbr <= 1) {               /* If single process, switch immediately to sequential mode */
    if (dgraphGather (&grafptr->s, &grafdat.data.cgrfdat) != 0) {
      errorPrint ("kdgraphMapRbPart: cannot centralize graph");
      return     (1);
    }
    return (kdgraphMapRbPartSequ (&grafdat, mappptr->mappptr, paraptr->stratseq));
  }

  grafdat.data.dgrfdat = grafptr->s;              /* Create a clone graph that will never be freed */
  grafdat.data.dgrfdat.flagval &= ~DGRAPHFREEALL;
  return (kdgraphMapRbPart2 (&grafdat, mappptr->mappptr, paraptr)); /* Perform DRB */
}
