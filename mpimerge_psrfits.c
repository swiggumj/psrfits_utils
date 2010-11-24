#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include "psrfits.h"
#include "mpi.h"

#define HDRLEN 14400

void reorder_data(unsigned char* outbuf, unsigned char *inbuf, 
                  int nband, int nspec, int npol, int nchan)
{
    int band, spec, pol, inoff = 0, outoff = 0;
    int spband = nspec * npol * nchan;
    int spspec = npol * nchan;

    for (spec = 0 ; spec < nspec ; spec++) {
        for (pol = 0 ; pol < npol ; pol++) {
            for (band = 0 ; band < nband ; band++) {
                inoff = band * spband + pol * nchan + spec * spspec;
                memcpy(outbuf + outoff, inbuf + inoff, nchan);
                outoff += nchan;
            }
        }
    }    
}

void usage() {
    printf("usage:  mpimerge_psrfits [optios] basefilename\n"
           "Options:\n"
           "  -o name, --output=name   Output base filename (auto-generate)\n"
           "  -i nn, --initial=nn      Starting input file number (1)\n"
           "  -f nn, --final=nn        Ending input file number (auto)\n"
           "\n");
}


int main(int argc, char *argv[])
{
    int ii, nc = 0, ncnp = 0, gpubps = 0, status = 0, statsum = 0;
    int fnum_start = 1, fnum_end = 0;
    int numprocs, numbands, myid;
    int *counts, *offsets;
    unsigned char *tmpbuf = NULL;
    struct psrfits pf;
    char hostname[100];
    char output_base[256] = "\0";
    MPI_Status mpistat;
    /* Cmd line */
    static struct option long_opts[] = {
        {"output",  1, NULL, 'o'},
        {"initial", 1, NULL, 'i'},
        {"final",   1, NULL, 'f'},
        {0,0,0,0}
    };
    int opt, opti;

    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &numprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);
    numbands = numprocs - 1;

    // Process the command line
    while ((opt=getopt_long(argc,argv,"o:i:f:",long_opts,&opti))!=-1) {
        switch (opt) {
        case 'o':
            strncpy(output_base, optarg, 255);
            output_base[255]='\0';
            break;
        case 'i':
            fnum_start = atoi(optarg);
            break;
        case 'f':
            fnum_end = atoi(optarg);
            break;
        default:
            if (myid==0) usage();
            MPI_Finalize();
            exit(0);
            break;
        }
    }
    if (optind==argc) { 
        if (myid==0) usage();
        MPI_Finalize();
        exit(1);
    }
    
    if (myid == 0) { // Master proc only
        printf("\n\n");
        printf("      MPI Search-mode PSRFITs Combiner\n");
        printf("              by Scott M. Ransom\n\n");
    }

    // Determine the hostnames of the processes
    {
        FILE *hostfile;
        
        hostfile = fopen("/etc/hostname", "r");
        fscanf(hostfile, "%s\n", hostname);
        fclose(hostfile);
        if (hostname != NULL) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (myid == 0) printf("\n");
            fflush(NULL);
            for (ii = 0 ; ii < numprocs ; ii++) {
                MPI_Barrier(MPI_COMM_WORLD);
                if (myid == ii)
                    printf("Process %3d is on machine %s\n", myid, hostname);
                fflush(NULL);
                MPI_Barrier(MPI_COMM_WORLD);
            }
            MPI_Barrier(MPI_COMM_WORLD);
            fflush(NULL);
        }
    }
    
    // Basefilenames for the GPU nodes
    if (myid > 0) {
        sprintf(pf.basefilename, "/data/gpu/partial/%s/%s", 
                hostname, argv[optind]);
    }

    // Initialize some key parts of the PSRFITS structure
    pf.tot_rows = pf.N = pf.T = pf.status = 0;
    pf.filenum = fnum_start;
    pf.filename[0] = '\0';

    if (myid == 1) {
        FILE *psrfitsfile;
        char hdr[HDRLEN], filenm[200];

        // Read the header info
        sprintf(filenm, "%s_0001.fits", pf.basefilename);
        psrfitsfile = fopen(filenm, "r");
        fread(&hdr, 1, HDRLEN, psrfitsfile);
        fclose(psrfitsfile);

        // Send the header to the master proc
        MPI_Send(hdr, HDRLEN, MPI_CHAR, 0, 0, MPI_COMM_WORLD);

    } else if (myid == 0) {
        FILE *psrfitsfile;
        char hdr[HDRLEN], tmpfilenm[80];

        // Receive the header info from proc 1
        MPI_Recv(hdr, HDRLEN, MPI_CHAR, 1, 0, MPI_COMM_WORLD, &mpistat);

        // Now write that header to a temp file
        strcpy(tmpfilenm, "mpi_merge_psrfits.XXXXXX");
        mkstemp(tmpfilenm); 
        psrfitsfile = fopen(tmpfilenm, "w");
        fwrite(&hdr, 1, HDRLEN, psrfitsfile);
        fclose(psrfitsfile); 
        sprintf(pf.filename, "%s", tmpfilenm);

        // And read the key information into a PSRFITS struct
        status = psrfits_open(&pf);
        status = psrfits_close(&pf);
        remove(tmpfilenm);

        // Now create the output PSTFITS file
        if (output_base[0]=='\0') {
            /* Set up default output filename */
            strcpy(output_base, argv[optind]);
        }
        strcpy(pf.basefilename, output_base);
        pf.filenum = 0;
        pf.multifile = 1;
        pf.filename[0] = '\0';
        nc = pf.hdr.nchan;
        ncnp = pf.hdr.nchan * pf.hdr.npol;
        gpubps = pf.sub.bytes_per_subint;
        pf.hdr.orig_nchan *= numbands;
        pf.hdr.nchan *= numbands;
        pf.hdr.fctr = pf.hdr.fctr - 0.5 * pf.hdr.BW + numbands/2.0 * pf.hdr.BW;
        pf.hdr.BW *= numbands;
        pf.sub.bytes_per_subint *= numbands;
        long long filelen = 40 * (1L<<30);  // In GB
        pf.rows_per_file = filelen / pf.sub.bytes_per_subint;
        status = psrfits_create(&pf);
        // For in-memory transpose of data
        tmpbuf = (unsigned char *)malloc(pf.sub.bytes_per_subint);
    }

    // Open the input PSRFITs files for real
    if (myid > 0) {
        status = psrfits_open(&pf);
        nc = pf.hdr.nchan;
        ncnp = pf.hdr.nchan * pf.hdr.npol;
        gpubps = pf.sub.bytes_per_subint;
    }

    // Alloc data buffers for the PSRFITS files
    pf.sub.dat_freqs = (float *)malloc(sizeof(float) * pf.hdr.nchan);
    pf.sub.dat_weights = (float *)malloc(sizeof(float) * pf.hdr.nchan);
    pf.sub.dat_offsets = (float *)malloc(sizeof(float) * 
                                         pf.hdr.nchan * pf.hdr.npol);
    pf.sub.dat_scales  = (float *)malloc(sizeof(float) * 
                                         pf.hdr.nchan * pf.hdr.npol);
    pf.sub.data = (unsigned char *)malloc(pf.sub.bytes_per_subint);

    // Counts and offsets for MPI_Gatherv
    counts = (int *)malloc(sizeof(int) * numprocs);
    offsets = (int *)malloc(sizeof(int) * numprocs);
    counts[0] = offsets[0] = 0;  //  master sends nothing

    // Now loop over the rows (i.e. subints)...
    do {
        MPI_Barrier(MPI_COMM_WORLD);

        // Read the current subint from each of the "slave" nodes
        if (myid > 0) {
            status = psrfits_read_subint(&pf);
            if (status==108) {
                printf("Proc %d:  Ignoring PSRFITS read error (%d).  Filling with zeros.\n", myid, status);
                // Set the data and the weights to all zeros
                for (ii = 0 ; ii < pf.hdr.nchan ; ii++) 
                    pf.sub.dat_weights[ii] = 0.0;
                for (ii = 0 ; ii < pf.sub.bytes_per_subint ; ii++) 
                    pf.sub.data[ii] = 0;
                // And the scales and offsets to nominal values
                for (ii = 0 ; ii < pf.hdr.nchan * pf.hdr.npol ; ii++) {
                    pf.sub.dat_offsets[ii] = 0.0;
                    pf.sub.dat_scales[ii]  = 1.0;
                }
                // reset the status to 0 and allow going to next row
                status = 0;
                pf.rownum++;
                pf.tot_rows++;
                pf.N += pf.hdr.nsblk;
                pf.T = pf.N * pf.hdr.dt;
            }
        }

        /* If we've passed final file, exit */
        if (fnum_end && pf.filenum > fnum_end) break;

        // Combine statuses of all nodes by summing....
        MPI_Allreduce(&status, &statsum, 1, 
                      MPI_INT, MPI_SUM, MPI_COMM_WORLD);
        if (statsum) break;
            
        if (myid == 1) { // Send all of the non-band-specific parts to master
            MPI_Send(&pf.sub.tsubint, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.offs, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.lst, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.ra, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.dec, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.glon, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.glat, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.feed_ang, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.pos_ang, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.par_ang, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.tel_az, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&pf.sub.tel_zen, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
        } else if (myid == 0) { // Receive all of the non-data parts
            MPI_Recv(&pf.sub.tsubint, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.offs, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.lst, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.ra, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.dec, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.glon, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.glat, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.feed_ang, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.pos_ang, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.par_ang, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.tel_az, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
            MPI_Recv(&pf.sub.tel_zen, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &mpistat);
        }

        // Now gather the vector quantities...

        // Vectors of length nchan
        for (ii = 1 ; ii < numprocs ; ii++) {
            counts[ii] = nc;
            offsets[ii] = (ii - 1) * nc;
        }
        status = MPI_Gatherv(pf.sub.dat_freqs, nc, MPI_FLOAT, 
                             pf.sub.dat_freqs, counts, offsets, MPI_FLOAT, 
                             0, MPI_COMM_WORLD);
        status = MPI_Gatherv(pf.sub.dat_weights, nc, MPI_FLOAT, 
                             pf.sub.dat_weights, counts, offsets, MPI_FLOAT, 
                             0, MPI_COMM_WORLD);
        
        // Vectors of length nchan * npol
        for (ii = 1 ; ii < numprocs ; ii++) {
            counts[ii] = ncnp;
            offsets[ii] = (ii - 1) * ncnp;
        }
        status = MPI_Gatherv(pf.sub.dat_offsets, ncnp, MPI_FLOAT, 
                             pf.sub.dat_offsets, counts, offsets, MPI_FLOAT, 
                             0, MPI_COMM_WORLD);
        status = MPI_Gatherv(pf.sub.dat_scales, ncnp, MPI_FLOAT, 
                             pf.sub.dat_scales, counts, offsets, MPI_FLOAT, 
                             0, MPI_COMM_WORLD);

        // Vectors of length pf.sub.bytes_per_subint for the raw data
        for (ii = 1 ; ii < numprocs ; ii++) {
            counts[ii] = gpubps;
            offsets[ii] = (ii - 1) * gpubps;
        }
        status = MPI_Gatherv(pf.sub.data, gpubps, MPI_UNSIGNED_CHAR, 
                             tmpbuf, counts, offsets, MPI_UNSIGNED_CHAR, 
                             0, MPI_COMM_WORLD);

        // Reorder and write the new row to the output file
        if (myid == 0) {
            reorder_data(pf.sub.data, tmpbuf, numbands, 
                         pf.hdr.nsblk, pf.hdr.npol, nc);
            status = psrfits_write_subint(&pf);
        }

    } while (statsum == 0);

    // Free the arrays
    free(pf.sub.dat_freqs);
    free(pf.sub.dat_weights);
    free(pf.sub.dat_offsets);
    free(pf.sub.dat_scales);
    free(pf.sub.data);
    if (myid == 0) free(tmpbuf);
    free(counts);
    free(offsets);
    
    // Close the files and finalize things
    status = psrfits_close(&pf);
    MPI_Finalize();
    exit(0);
}
