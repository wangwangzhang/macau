#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <iostream>

#include <fstream>
#include <string>
#include <algorithm>
#include <random>
#include <chrono>
#include <memory>
#include <cmath>

#include <unsupported/Eigen/SparseExtra>
#include <Eigen/Sparse>

#include <omp.h>
#include <signal.h>

#include "macau.h"
#include "mvnormal.h"
#include "bpmfutils.h"
#include "latentprior.h"
#include "noisemodels.h"
#include "sparsetensor.h"
#include "eigen3-hdf5.hpp"

using namespace std; 
using namespace Eigen;

static volatile bool keepRunning = true;

void intHandler(int dummy) {
  keepRunning = false;
  printf("[Received Ctrl-C. Stopping after finishing the current iteration.]\n");
}

template<class DType, class NType> 
void MacauX<DType, NType>::addPrior(unique_ptr<ILatentPrior> & prior) {
  priors.push_back( std::move(prior) );
}

template<class DType, class NType> 
void MacauX<DType, NType>::setSamples(int b, int n) {
  burnin = b;
  nsamples = n;
}

template<class DType, class NType> 
void MacauX<DType, NType>::setRelationData(int* rows, int* cols, double* values, int N, int nrows, int ncols) {
  data.setTrain(rows, cols, values, N, nrows, ncols);
}

template<class DType, class NType> 
void MacauX<DType, NType>::setRelationDataTest(int* rows, int* cols, double* values, int N, int nrows, int ncols) {
  data.setTest(rows, cols, values, N, nrows, ncols);
}

template<class DType, class NType> 
void MacauX<DType, NType>::setRelationData(int* idx, int nmodes, double* values, int nnz, int* dims) {
  data.setTrain(idx, nmodes, values, nnz, dims);
}

template<class DType, class NType> 
void MacauX<DType, NType>::setRelationDataTest(int* idx, int nmodes, double* values, int nnz, int* dims) {
  data.setTest(idx, nmodes, values, nnz, dims);
}

template<class DType, class NType> 
double MacauX<DType, NType>::getRmseTest() { return rmse_test; }

template<class DType, class NType> 
void MacauX<DType, NType>::init() {
  unsigned seed1 = std::chrono::system_clock::now().time_since_epoch().count();
  if (priors.size() < 2) {
    throw std::runtime_error("Less 2 priors are not supported.");
  }
  init_bmrng(seed1);
  VectorXi dims = data.getDims();
  for (int mode = 0; mode < dims.size(); mode++) {
    MatrixXd* U = new MatrixXd(num_latent, dims(mode));
    U->setZero();
    samples.push_back( std::move(std::unique_ptr<MatrixXd>(U)) );
  }
  noise.init(data);
  keepRunning = true;
}

Macau::~Macau() {
}

inline double sqr(double x) { return x*x; }

template<class DType, class NType> 
void MacauX<DType, NType>::run() {
  init();
  if (verbose) {
    std::cout << noise.getInitStatus() << endl;
    std::cout << "Sampling" << endl;
  }
  if (save_model) {
    saveGlobalParams();
  }
  signal(SIGINT, intHandler);

  const VectorXi dims = data.getDims();
  predictions     = VectorXd::Zero( data.getTestNonzeros() );
  predictions_var = VectorXd::Zero( data.getTestNonzeros() );

  auto start = tick();
  for (int i = 0; i < burnin + nsamples; i++) {
    if (keepRunning == false) {
      keepRunning = true;
      break;
    }
    if (verbose && i == burnin) {
      printf(" ====== Burn-in complete, averaging samples ====== \n");
    }
    auto starti = tick();

    // sample latent vectors
    for (int mode = 0; mode < dims.size(); mode++) {
      priors[mode]->sample_latents(noise, data, samples, mode, num_latent);
    }

    // Sample hyperparams
    for (int mode = 0; mode < dims.size(); mode++) {
      priors[mode]->update_prior(*samples[mode]);
    }

    noise.update(data, samples);

    noise.evalModel(data, (i < burnin) ? 0 : (i - burnin), predictions, predictions_var, samples);
    

    auto endi = tick();
    auto elapsed = endi - start;
    double samples_per_sec = (i + 1) * (dims.sum()) / elapsed;
    double elapsedi = endi - starti;

    if (save_model && i >= burnin) {
      saveModel(i - burnin + 1);
    }
    if (verbose) {
      printStatus(i, elapsedi, samples_per_sec);
    }
    rmse_test = noise.getEvalMetric();
  }
}

template<class DType, class NType> 
void MacauX<DType, NType>::printStatus(int i, double elapsedi, double samples_per_sec) {
  printf("Iter %3d: %s  ", i, noise.getEvalString().c_str());

  printf("U:[");
  for (unsigned p = 0; p < priors.size(); p++) {
    printf("%1.2e", samples[p]->norm());
    if (p + 1 < priors.size()) {
      printf(", ");
    }
  }
  printf("]");

  printf("\tSide:[");
  for (unsigned p = 0; p < priors.size(); p++) {
    printf("%1.2e", priors[p]->getLinkNorm());
    if (p + 1 < priors.size()) {
      printf(", ");
    }
  }
  printf("]");
  printf("%s [took %0.1fs]\n", noise.getStatus().c_str(), elapsedi);
}

template<class DType, class NType> 
Eigen::VectorXd MacauX<DType, NType>::getStds() {
  VectorXd std( data.getTestNonzeros() );
  if (nsamples <= 1) {
    std.setConstant(NAN);
    return std;
  }
  const int n = std.size();
  const double inorm = 1.0 / (nsamples - 1);
#pragma omp parallel for schedule(static)
  for (int i = 0; i < n; i++) {
    std[i] = sqrt(predictions_var[i] * inorm);
  }
  return std;
}

template<class DType, class NType> 
void MacauX<DType, NType>::saveModel(int isample) {
  string fprefix = save_prefix + "-sample" + std::to_string(isample) + "-";
  // saving latent matrices
  for (unsigned int i = 0; i < samples.size(); i++) {
    //writeToCSVfile(fprefix + "U" + std::to_string(i+1) + "-latents.csv", *samples[i]);
	writeToHdf5file(save_prefix, fprefix + "U" + std::to_string(i+1) + "-latents", *samples[i]);
    priors[i]->saveModel(save_prefix, fprefix + "U" + std::to_string(i+1));
	
  }
}

template<class DType, class NType> 
void MacauX<DType, NType>::saveGlobalParams() {
  VectorXd means(1);
  means << data.getMeanValue();
  Eigen::MatrixXd mat = means;
  //writeToCSVfile(save_prefix + "-meanvalue.csv", means);
  H5::H5File file( save_prefix, H5F_ACC_TRUNC );
  EigenHDF5::save(file, save_prefix + "-meanvalue", mat);
}

Macau* make_macau_probit(int nmodes, int num_latent) {
  switch (nmodes) {
    case 2: return new MacauX<MatrixData,  ProbitNoise>(num_latent, MatrixData(), ProbitNoise());
    default: return new MacauX<TensorData, ProbitNoise>(num_latent, TensorData(nmodes), ProbitNoise());
  }
}

Macau* make_macau_fixed(int nmodes, int num_latent, double precision) {
  FixedGaussianNoise fnoise(precision);
  switch (nmodes) {
    case 2: return new MacauX<MatrixData,  FixedGaussianNoise>(num_latent, MatrixData(), fnoise);
    default: return new MacauX<TensorData, FixedGaussianNoise>(num_latent, TensorData(nmodes), fnoise);
  }
}

Macau* make_macau_adaptive(int nmodes, int num_latent, double sn_init, double sn_max) {
  AdaptiveGaussianNoise anoise(sn_init, sn_max);
  switch (nmodes) {
    case 2: return new MacauX<MatrixData,  AdaptiveGaussianNoise>(num_latent, MatrixData(), anoise);
    default: return new MacauX<TensorData, AdaptiveGaussianNoise>(num_latent, TensorData(nmodes), anoise);
  }
}

template class MacauX<MatrixData, FixedGaussianNoise>;
template class MacauX<MatrixData, AdaptiveGaussianNoise>;
template class MacauX<MatrixData, ProbitNoise>;

template class MacauX<TensorData, FixedGaussianNoise>;
template class MacauX<TensorData, AdaptiveGaussianNoise>;
template class MacauX<TensorData, ProbitNoise>;
