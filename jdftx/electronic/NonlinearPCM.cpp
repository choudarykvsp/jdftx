/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman, Kendra Letchworth Weaver, Deniz Gunceler

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <electronic/Everything.h>
#include <electronic/NonlinearPCM.h>
#include <electronic/PCM_internal.h>
#include <core/Units.h>
#include <core/DataIO.h>
#include <core/Operators.h>
#include <electronic/operators.h>
#include <core/Util.h>
#include <core/DataMultiplet.h>

//Utility functions to extract/set the members of a MuEps
inline DataRptr& getMuPlus(DataRMuEps& X) { return X[0]; }
inline const DataRptr& getMuPlus(const DataRMuEps& X) { return X[0]; }
inline DataRptr& getMuMinus(DataRMuEps& X) { return X[1]; }
inline const DataRptr& getMuMinus(const DataRMuEps& X) { return X[1]; }
inline DataRptrVec getEps(DataRMuEps& X) { return DataRptrVec(X.component+2); }
inline const DataRptrVec getEps(const DataRMuEps& X) { return DataRptrVec(X.component+2); }
inline void setMuEps(DataRMuEps& mueps, DataRptr muPlus, DataRptr muMinus, DataRptrVec eps) { mueps[0]=muPlus; mueps[1]=muMinus; for(int k=0; k<3; k++) mueps[k+2]=eps[k]; }


inline void setPreconditioner(int i, double G2, double* preconditioner, double kappaSqByEpsilon, double muByEpsSq)
{	double den = G2 + kappaSqByEpsilon;
	preconditioner[i] = den ? muByEpsSq*G2/(den*den) : 0.;
}

NonlinearPCM::NonlinearPCM(const Everything& e, const FluidSolverParams& fsp)
: FluidSolver(e), params(fsp), preconditioner(e.gInfo)
{
	//Initialize dielectric evaluation class:
	dielectricEval = new NonlinearPCMeval::Dielectric(params.linearDielectric,
		params.T, params.Nbulk, params.pMol, params.epsilonBulk, params.epsInf);
	
	//Optionally initialize screening evaluation class:
	screeningEval = 0;
	if(params.ionicConcentration)
		screeningEval = new NonlinearPCMeval::Screening(params.linearScreening,
			params.T, params.ionicConcentration, params.ionicZelectrolyte,
			params.ionicRadiusPlus, params.ionicRadiusMinus, params.epsilonBulk);
	
	//Initialize preconditioner (for mu channel):
	double muByEps = (params.ionicZelectrolyte/params.pMol) * (1.-dielectricEval->alpha/3); //relative scale between mu and eps
	applyFuncGsq(e.gInfo, setPreconditioner, preconditioner.data, params.k2factor/params.epsilonBulk, muByEps*muByEps);
	preconditioner.set();
}

NonlinearPCM::~NonlinearPCM()
{
	delete dielectricEval;
	if(screeningEval)
		delete screeningEval;
}


void NonlinearPCM::set(const DataGptr& rhoExplicitTilde, const DataGptr& nCavityTilde)
{	
	//Initialize state if required:
	if(!state) //nullToZero(state, e.gInfo);
	{	logPrintf("Initializing state of NonlinearPCM using a similar LinearPCM:\n");
		FILE*& fpLog = ((MinimizeParams&)e.fluidMinParams).fpLog;
		fpLog = fopen("/dev/null", "w"); //disable iteration log from LinearPCM
		LinearPCM linearPCM(e, params);
		linearPCM.set(rhoExplicitTilde, nCavityTilde);
		linearPCM.minimizeFluid();
		fclose(fpLog);
		fpLog = globalLog; //retsore usual iteration log
		//Guess nonlinear states based on the electrostatic potential of the linear version:
		//mu:
		DataRptr mu;
		if(screeningEval && screeningEval->linear)
		{	mu = (-params.ionicZelectrolyte/params.T) * I(linearPCM.state);
			mu -= integral(mu)/e.gInfo.detR; //project out G=0
		}
		else initZero(mu, e.gInfo); //initialization logic does not work well with hard sphere limit
		//eps:
		DataRptrVec eps = (-params.pMol/params.T) * I(gradient(linearPCM.state));
		DataRptr E = sqrt(eps[0]*eps[0] + eps[1]*eps[1] + eps[2]*eps[2]);
		DataRptr Ecomb = 0.5*((dielectricEval->alpha-3.) + E);
		DataRptr epsByE = inv(E) * (Ecomb + sqrt(Ecomb*Ecomb + 3.*E));
		eps *= epsByE; //enhancement due to correlations
		//collect:
		setMuEps(state, mu, clone(mu), eps);
	}
	
	this->rhoExplicitTilde = rhoExplicitTilde;
	this->nCavity = I(nCavityTilde);

	//Initialize shape function
	nullToZero(shape,e.gInfo);
	pcmShapeFunc(nCavity, shape, params.nc, params.sigma);

	//Compute the cavitation energy and gradient
	Acavity = cavitationEnergyAndGrad(shape, Acavity_shape, params.cavityTension, params.cavityPressure);
}

double NonlinearPCM::operator()(const DataRMuEps& state, DataRMuEps& Adiel_state, DataGptr* Adiel_rhoExplicitTilde, DataGptr* Adiel_nCavityTilde) const
{
	DataRptr Adiel_shape; if(Adiel_nCavityTilde) nullToZero(Adiel_shape, e.gInfo);
	
	DataGptr rhoFluidTilde;
	DataRptr muPlus, muMinus, Adiel_muPlus, Adiel_muMinus;
	initZero(Adiel_muPlus, e.gInfo); initZero(Adiel_muMinus, e.gInfo);
	double Akappa = 0., mu0 = 0., Qexp = 0., Adiel_Qexp = 0.;
	if(screeningEval)
	{	//Get neutrality Lagrange multiplier:
		muPlus = getMuPlus(state);
		muMinus = getMuMinus(state);
		Qexp = integral(rhoExplicitTilde);
		mu0 = screeningEval->neutralityConstraint(muPlus, muMinus, shape, Qexp);
		//Compute ionic free energy and bound charge
		DataRptr Aout, rhoIon;
		initZero(Aout, e.gInfo);
		initZero(rhoIon, e.gInfo);
		callPref(screeningEval->freeEnergy)(e.gInfo.nr, mu0, muPlus->dataPref(), muMinus->dataPref(), shape->dataPref(),
			rhoIon->dataPref(), Aout->dataPref(), Adiel_muPlus->dataPref(), Adiel_muMinus->dataPref(), Adiel_shape ? Adiel_shape->dataPref() : 0);
		Akappa = integral(Aout);
		rhoFluidTilde += J(rhoIon); //include bound charge due to ions
	}
	
	//Compute the dielectric free energy and bound charge:
	DataRptrVec eps = getEps(state), Adiel_eps; double Aeps = 0.;
	{	DataRptr Aout; DataRptrVec p;
		initZero(Aout, e.gInfo);
		nullToZero(p, e.gInfo);
		nullToZero(Adiel_eps, e.gInfo);
		callPref(dielectricEval->freeEnergy)(e.gInfo.nr, eps.const_dataPref(), shape->dataPref(),
			p.dataPref(), Aout->dataPref(), Adiel_eps.dataPref(), Adiel_shape ? Adiel_shape->dataPref() : 0);
		Aeps = integral(Aout);
		rhoFluidTilde -= divergence(J(p)); //include bound charge due to dielectric
	} //scoped to automatically deallocate temporaries
	
	//Compute the electrostatic terms:
	DataGptr phiFluidTilde = (*e.coulomb)(rhoFluidTilde);
	DataGptr phiExplicitTilde = (*e.coulomb)(rhoExplicitTilde);
	double U = dot(rhoFluidTilde, O(0.5*phiFluidTilde + phiExplicitTilde));
	
	if(screeningEval)
	{	//Propagate gradients from rhoIon to mu, shape
		DataRptr Adiel_rhoIon = I(phiFluidTilde+phiExplicitTilde);
		callPref(screeningEval->convertDerivative)(e.gInfo.nr, mu0, muPlus->dataPref(), muMinus->dataPref(), shape->dataPref(),
			Adiel_rhoIon->dataPref(), Adiel_muPlus->dataPref(), Adiel_muMinus->dataPref(), Adiel_shape ? Adiel_shape->dataPref() : 0);
		//Propagate gradients from mu0 to mu, shape, Qexp:
		double Adiel_mu0 = integral(Adiel_muPlus) + integral(Adiel_muMinus), mu0_Qexp;
		DataRptr mu0_muPlus, mu0_muMinus, mu0_shape;
		screeningEval->neutralityConstraint(muPlus, muMinus, shape, Qexp, &mu0_muPlus, &mu0_muMinus, &mu0_shape, &mu0_Qexp);
		Adiel_muPlus += Adiel_mu0 * mu0_muPlus;
		Adiel_muMinus += Adiel_mu0 * mu0_muMinus;
		if(Adiel_shape) Adiel_shape += Adiel_mu0 * mu0_shape;
		Adiel_Qexp = Adiel_mu0 * mu0_Qexp;
	}
	
	//Propagate gradients from p to eps, shape
	{	DataRptrVec Adiel_p = I(gradient(phiFluidTilde+phiExplicitTilde), true); //Because dagger(-divergence) = gradient
		callPref(dielectricEval->convertDerivative)(e.gInfo.nr, eps.const_dataPref(), shape->dataPref(),
			Adiel_p.const_dataPref(), Adiel_eps.dataPref(), Adiel_shape ? Adiel_shape->dataPref() : 0);
	}
	
	//Optional outputs:
	if(Adiel_rhoExplicitTilde)
	{	*Adiel_rhoExplicitTilde = phiFluidTilde;
		(*Adiel_rhoExplicitTilde)->setGzero(Adiel_Qexp);
	}
	if(Adiel_nCavityTilde)
	{	Adiel_shape  += Acavity_shape; // Add the cavitation contribution to Adiel_shape
		DataRptr Adiel_nCavity(DataR::alloc(e.gInfo));
		pcmShapeFunc_grad(nCavity, Adiel_shape, Adiel_nCavity, params.nc, params.sigma);
		*Adiel_nCavityTilde = J(Adiel_nCavity);
	}
	
	//Collect energy and gradient pieces:
	setMuEps(Adiel_state, Adiel_muPlus, Adiel_muMinus, Adiel_eps);
	Adiel_state *= e.gInfo.dV; //converts variational derivative to total derivative
	return Akappa + Aeps + U + Acavity;
}

void NonlinearPCM::minimizeFluid()
{	minimize(e.fluidMinParams);
}

void NonlinearPCM::loadState(const char* filename)
{	nullToZero(state, e.gInfo);
	state.loadFromFile(filename);
}

void NonlinearPCM::saveState(const char* filename) const
{	state.saveToFile(filename);
}

double NonlinearPCM::get_Adiel_and_grad(DataGptr& Adiel_rhoExplicitTilde, DataGptr& Adiel_nCavityTilde, IonicGradient& extraForces)
{	DataRMuEps Adiel_state;
	return (*this)(state, Adiel_state, &Adiel_rhoExplicitTilde, &Adiel_nCavityTilde);
}

void NonlinearPCM::step(const DataRMuEps& dir, double alpha)
{	axpy(alpha, dir, state);
}

double NonlinearPCM::compute(DataRMuEps* grad)
{	DataRMuEps gradUnused;
	return (*this)(state, grad ? *grad : gradUnused);
}

DataRMuEps NonlinearPCM::precondition(const DataRMuEps& in)
{	DataRMuEps out;
	setMuEps(out,
		I(preconditioner*J(getMuPlus(in))),
		I(preconditioner*J(getMuMinus(in))),
		getEps(in));
	return out;
}

void rx(int i, vector3<> r, int dir, matrix3<> R, double* rx)
{	vector3<> lx = inv(R)*r;
	for(int j = 0; j<3; j++)
		lx[j] = lx[j]<0.5 ? lx[j] : lx[j]-1.;
	rx[i] = (R*lx)[dir];
}


void NonlinearPCM::dumpDebug(const char* filenamePattern) const
{	
	// Prepares to dump
	string filename(filenamePattern);
	filename.replace(filename.find("%s"), 2, "Debug");
	logPrintf("Dumping '%s'... \t", filename.c_str());  logFlush();

	FILE* fp = fopen(filename.c_str(), "w");
	if(!fp) die("Error opening %s for writing.\n", filename.c_str());	

	// Dumps the polarization fraction
	DataRptrVec shape_x = gradient(shape);
	DataRptr surfaceDensity = sqrt(shape_x[0]*shape_x[0] + shape_x[1]*shape_x[1] + shape_x[2]*shape_x[2]);
	DataRptrVec eps = getEps(state);
	DataRptr eps_mag = sqrt(eps[0]*eps[0] + eps[1]*eps[1] + eps[2]*eps[2]);
	double Eaveraged = integral(eps_mag*shape*surfaceDensity)/integral(surfaceDensity);
	double Eaveraged2 = integral(eps_mag*surfaceDensity)/integral(surfaceDensity);
	
	fprintf(fp, "Cavity volume = %f\n", integral(1.-shape));
	fprintf(fp, "Cavity surface Area = %f\n", integral(surfaceDensity));
	
	fprintf(fp, "\nSurface averaged epsilon: %f", Eaveraged);
	fprintf(fp, "\nSurface averaged epsilon (no shape weighing): %f\n", Eaveraged2);

	fclose(fp);	
	logPrintf("done\n"); logFlush();
	
}

void NonlinearPCM::dumpDensities(const char* filenamePattern) const
{
	string filename;
	#define DUMP(object, suffix) \
		filename = filenamePattern; \
		filename.replace(filename.find("%s"), 2, suffix); \
		logPrintf("Dumping '%s'... ", filename.c_str());  logFlush(); \
		saveRawBinary(object, filename.c_str()); \
		logPrintf("done.\n"); logFlush();
	DUMP(shape, "Shape");

	if(screeningEval)
	{
		DataRptr Nplus, Nminus;
		{	DataRptr muPlus = getMuPlus(state);
			DataRptr muMinus = getMuMinus(state);
			double Qexp = integral(rhoExplicitTilde);
			double mu0 = screeningEval->neutralityConstraint(muPlus, muMinus, shape, Qexp);
			Nplus = params.ionicConcentration * shape * (params.linearScreening ? 1.+(mu0+muPlus) : exp(mu0+muPlus));
			Nminus = params.ionicConcentration * shape * (params.linearScreening ? 1.-(mu0+muMinus) : exp(-(mu0+muMinus)));
		}
		DUMP(Nplus, "N+");
		DUMP(Nminus, "N-");
	}
}
