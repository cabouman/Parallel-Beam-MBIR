/* ============================================================================== 
 * Copyright (c) 2016 Xiao Wang (Purdue University)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or
 * other materials provided with the distribution.
 *
 * Neither the name of Xiao Wang, Purdue University,
 * nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ============================================================================== */

#include <math.h>

#include "../../../Utilities/3D/MBIRModularUtils_3D.h"
#include "../../../Utilities/2D/MBIRModularUtils_2D.h"
#include "icd3d.h"
#include <stdlib.h>
#include <stdio.h>


float ICDStep3D(
	float **e,  /* e=y-AX */
	float **w,
    struct SysMatrix2D *A,
	struct ICDInfo *icd_info)
{
	int i, n, Nxy, XYPixelIndex, SliceIndex;
    struct SparseColumn A_column;
	float UpdatedVoxelValue;

    Nxy = icd_info->Nxy; /* No. of pixels within a given slice */
    
    /* Voxel Index: jz*Nx*Ny + jy*Nx + jx */
    XYPixelIndex = icd_info->VoxelIndex%Nxy; /* XY pixel index within a given slice */
    SliceIndex = icd_info->VoxelIndex/Nxy;   /* Index of slice : between 0 to NSlices-1 */
    
    A_column = A->column[XYPixelIndex]; /* System matrix does not vary with slice for 3-D Parallel beam geometry */
    
    /* Formulate the quadratic surrogate function (with coefficients theta1, theta2) for the local cost function */
	icd_info->theta1 = 0.0;
	icd_info->theta2 = 0.0;
    
	for (n = 0; n < A_column.Nnonzero; n++)
	{
		i = A_column.RowIndex[n] ; /* (View, Detector-Channel) index pertaining to same slice as voxel */
        
        icd_info->theta1 -= A_column.Value[n]*w[SliceIndex][i]*e[SliceIndex][i];
        icd_info->theta2 += A_column.Value[n]*w[SliceIndex][i]*A_column.Value[n];
	}
   
    /* theta1 and theta2 must be further adjusted according to Prior Model */
    /* Step can be skipped if merely ML estimation (no prior model) is followed rather than MAP estimation */
    QGGMRF3D_UpdateICDParams(icd_info);
	
    /* Calculate Updated Pixel Value */
    UpdatedVoxelValue = icd_info->v - (icd_info->theta1/icd_info->theta2) ;
    
	return UpdatedVoxelValue;
}

/* ICD update with the QGGMRF prior model */
/* Prior and neighborhood specific */
void QGGMRF3D_UpdateICDParams(struct ICDInfo *icd_info)
{
    int j; /* Neighbor relative position to Pixel being updated */
    float delta, SurrogateCoeff;
    float sum1_Nearest=0, sum1_Diag=0, sum1_Interslice=0; /* for theta1 calculation */
    float sum2_Nearest=0, sum2_Diag=0, sum2_Interslice=0; /* for theta2 calculation */
    float b_nearest, b_diag, b_interslice;
    
    b_nearest=icd_info->Rparams.b_nearest;
    b_diag=icd_info->Rparams.b_diag;
    b_interslice = icd_info->Rparams.b_interslice;
    
    for (j = 0; j < 10; j++)
    {
        delta = icd_info->v - icd_info->neighbors[j];
        SurrogateCoeff = QGGMRF_SurrogateCoeff(delta,icd_info);
        
        if (j < 4)
        {
            sum1_Nearest += (SurrogateCoeff * delta);
            sum2_Nearest += SurrogateCoeff;
        }
        if(j>=4 && j<6)
        {
            sum1_Interslice += (SurrogateCoeff * delta);
            sum2_Interslice += SurrogateCoeff;
        }
        if (j >= 6)
        {
            sum1_Diag += (SurrogateCoeff * delta);
            sum2_Diag += SurrogateCoeff;
        }
    }
    
    icd_info->theta1 +=  (b_nearest * sum1_Nearest + b_diag * sum1_Diag + b_interslice * sum1_Interslice) ;
    icd_info->theta2 +=  (b_nearest * sum2_Nearest + b_diag * sum2_Diag + b_interslice * sum2_Interslice) ;
}


/* the potential function of the QGGMRF prior model.  p << q <= 2 */
float QGGMRF_Potential(float delta, struct ReconParamsQGGMRF3D *Rparams)
{
    float p, q, T, SigmaX;
    float temp, GGMRF_Pot;
    
    p = Rparams->p;
    q = Rparams->q;
    T = Rparams->T;
    SigmaX = Rparams->SigmaX;
    
    GGMRF_Pot = pow(fabs(delta),p)/(p*pow(SigmaX,p));
    temp = pow(fabs(delta/(T*SigmaX)), q-p);
    
    return ( GGMRF_Pot * temp/(1.0+temp) );
}

/* Quadratic Surrogate Function for the log(prior model) */
/* For a given convex potential function rho(delta) ... */
/* The surrogate function defined about a point "delta_p", Q(delta ; delta_p), is given by ... */
/* Q(delta ; delta_p) = a(delta_p) * (delta^2/2), where the coefficient a(delta_p) is ... */
/* a(delta_p) = [ rho'(delta_p)/delta_p ]   ... */
/* for the case delta_current is Non-Zero and rho' is the 1st derivative of the potential function */
/* Return this coefficient a(delta_p) */
/* Prior-model specific, independent of neighborhood */

float QGGMRF_SurrogateCoeff(float delta, struct ICDInfo *icd_info)
{
    float p, q, T, SigmaX, qmp;
    float num, denom, temp;
    
    p = icd_info->Rparams.p;
    q = icd_info->Rparams.q;
    T = icd_info->Rparams.T;
    SigmaX = icd_info->Rparams.SigmaX;
    qmp = q - p;
    
    /* Refer to Chapter 7, MBIR Textbook by Prof Bouman, Page 151 */
    /* Table on Quadratic surrogate functions for different prior models */
    
    if (delta == 0.0)
    return 2.0/( p*pow(SigmaX,q)*pow(T,qmp) ) ; /* rho"(0) */
    
    temp = pow(fabs(delta/(T*SigmaX)), qmp);
    num = q/p + temp;
    denom = pow(SigmaX,p) * (1.0+temp) * (1.0+temp);
    num = num * pow(fabs(delta),p-2) * temp;
    
    return num/denom;
}


/* extract the neighborhood system */
void ExtractNeighbors3D(
                        struct ICDInfo *icd_info,
                        struct Image3D *Image)
{
    int jx, jy, jz, plusx, minusx, plusy, minusy, plusz, minusz;
    int Nx, Ny, Nz, Nxy;
    
    Nx = Image->imgparams.Nx;
    Ny = Image->imgparams.Ny;
    Nz = Image->imgparams.Nz;
    Nxy = Nx*Ny;
    
    /* Voxel Index = jz*Ny*Nx+jy*Nx+jx */
    jz = icd_info->VoxelIndex/(Ny*Nx); /* Z-Index of pixel */
    jy = (icd_info->VoxelIndex/Nx)%Ny; /* Y-index of pixel */
    jx = icd_info->VoxelIndex%Nx;      /* X-index of pixel */
    
    plusx = jx + 1;
    plusx = ((plusx < Nx) ? plusx : 0);
    minusx = jx - 1;
    minusx = ((minusx < 0) ? (Nx-1) : minusx);
    plusy = jy + 1;
    plusy = ((plusy < Ny) ? plusy : 0);
    minusy = jy - 1;
    minusy = ((minusy < 0) ? (Ny-1) : minusy);
    plusz = jz + 1;
    plusz = ((plusz < Nz) ? plusz : 0);
    minusz = jz - 1;
    minusz = ((minusz < 0) ? (Nz-1) : minusz);
    
    icd_info->neighbors[0] = Image->image[jz][jy*Nx+plusx];
    icd_info->neighbors[1] = Image->image[jz][jy*Nx+minusx];
    icd_info->neighbors[2] = Image->image[jz][plusy*Nx+jx];
    icd_info->neighbors[3] = Image->image[jz][minusy*Nx+jx];
    
    icd_info->neighbors[4] = Image->image[plusz][jy*Nx+jx];
    icd_info->neighbors[5] = Image->image[minusz][jy*Nx+jx];
    
    icd_info->neighbors[6] = Image->image[jz][plusy*Nx+plusx];
    icd_info->neighbors[7] = Image->image[jz][plusy*Nx+minusx];
    icd_info->neighbors[8] = Image->image[jz][minusy*Nx+plusx];
    icd_info->neighbors[9] = Image->image[jz][minusy*Nx+minusx];
}

/* Update error term e=y-Ax after an ICD update on x */
void UpdateError3D(
                   float **e,
                   struct SysMatrix2D *A,
                   float diff,
                   struct ICDInfo *icd_info)
{
    int n, i, XYPixelIndex, SliceIndex;
    int Nxy;
    
    Nxy = icd_info->Nxy; /* No. of pixels within a given slice */
    
    /* Voxel Index: jz*Nx*Ny + jy*Nx + jx */
    XYPixelIndex = icd_info->VoxelIndex%Nxy; /* XY pixel index within a given slice */
    SliceIndex = icd_info->VoxelIndex/Nxy;   /* Index of slice : between 0 to NSlices-1 */
    
    /* System matrix does not vary with slice for 3-D Parallel beam geometry, so A->column only indexed by XYPixelIndex */
    /* Update sinogram error */
    for (n = 0; n < A->column[XYPixelIndex].Nnonzero; n++)
    {
        i = A->column[XYPixelIndex].RowIndex[n]  ; /* (View, Detector-Channel) index pertaining to same slice as voxel */
        e[SliceIndex][i] -= A->column[XYPixelIndex].Value[n]*diff;
    }
}



