//  UWOT -- An R package for dimensionality reduction using UMAP
//
//  Copyright (C) 2018 James Melville
//
//  This file is part of UWOT
//
//  UWOT is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  UWOT is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with UWOT.  If not, see <http://www.gnu.org/licenses/>.

#include <vector>

#include "uwot/coords.h"
#include "uwot/epoch.h"
#include "uwot/gradient.h"
#include "uwot/optimize.h"
#include "uwot/sampler.h"

#include "rng.h"
#include "rparallel.h"
#include "rprogress.h"

using namespace Rcpp;

template <typename T>
auto lget(List list, const std::string &name, T default_value) -> T {
  auto key = name.c_str();
  if (!list.containsElementNamed(key)) {
    return default_value;
  } else {
    return list[key];
  }
}

// Template class specialization to handle different rng/batch combinations
template <bool DoBatch = true> struct BatchRngFactory {
  using PcgFactoryType = batch_pcg_factory;
  using TauFactoryType = batch_tau_factory;
};
template <> struct BatchRngFactory<false> {
  using PcgFactoryType = pcg_factory;
  using TauFactoryType = tau_factory;
};

struct UmapFactory {
  bool move_other;
  bool pcg_rand;
  std::vector<float> &head_embedding;
  std::vector<float> &tail_embedding;
  const std::vector<unsigned int> &positive_head;
  const std::vector<unsigned int> &positive_tail;
  const std::vector<unsigned int> &positive_ptr;
  unsigned int n_epochs;
  unsigned int n_head_vertices;
  unsigned int n_tail_vertices;
  const std::vector<float> &epochs_per_sample;
  float initial_alpha;
  List opt_args;
  float negative_sample_rate;
  bool batch;
  std::size_t n_threads;
  std::size_t grain_size;
  uwot::EpochCallback *epoch_callback;
  bool verbose;

  UmapFactory(bool move_other, bool pcg_rand,
              std::vector<float> &head_embedding,
              std::vector<float> &tail_embedding,
              const std::vector<unsigned int> &positive_head,
              const std::vector<unsigned int> &positive_tail,
              const std::vector<unsigned int> &positive_ptr,
              unsigned int n_epochs, unsigned int n_head_vertices,
              unsigned int n_tail_vertices,
              const std::vector<float> &epochs_per_sample, float initial_alpha,
              List opt_args, float negative_sample_rate, bool batch,
              std::size_t n_threads, std::size_t grain_size,
              uwot::EpochCallback *epoch_callback, bool verbose)
      : move_other(move_other), pcg_rand(pcg_rand),
        head_embedding(head_embedding), tail_embedding(tail_embedding),
        positive_head(positive_head), positive_tail(positive_tail),
        positive_ptr(positive_ptr), n_epochs(n_epochs),
        n_head_vertices(n_head_vertices), n_tail_vertices(n_tail_vertices),
        epochs_per_sample(epochs_per_sample), initial_alpha(initial_alpha),
        opt_args(opt_args), negative_sample_rate(negative_sample_rate),
        batch(batch), n_threads(n_threads), grain_size(grain_size),
        epoch_callback(epoch_callback), verbose(verbose) {}

  template <typename Gradient> void create(const Gradient &gradient) {
    if (move_other) {
      create_impl<true>(gradient, pcg_rand, batch);
    } else {
      create_impl<false>(gradient, pcg_rand, batch);
    }
  }

  template <bool DoMove, typename Gradient>
  void create_impl(const Gradient &gradient, bool pcg_rand, bool batch) {
    if (batch) {
      create_impl<BatchRngFactory<true>, DoMove>(gradient, pcg_rand, batch);
    } else {
      create_impl<BatchRngFactory<false>, DoMove>(gradient, pcg_rand, batch);
    }
  }

  template <typename BatchRngFactory, bool DoMove, typename Gradient>
  void create_impl(const Gradient &gradient, bool pcg_rand, bool batch) {
    if (pcg_rand) {
      create_impl<typename BatchRngFactory::PcgFactoryType, DoMove>(gradient,
                                                                    batch);
    } else {
      create_impl<typename BatchRngFactory::TauFactoryType, DoMove>(gradient,
                                                                    batch);
    }
  }

  auto create_adam(List opt_args) -> uwot::Adam {
    float alpha = lget(opt_args, "alpha", 1.0);
    float beta1 = lget(opt_args, "beta1", 0.9);
    float beta2 = lget(opt_args, "beta2", 0.999);
    float eps = lget(opt_args, "eps", 1e-7);
    if (verbose) {
      Rcerr << "Optimizing with Adam"
            << " alpha = " << alpha << " beta1 = " << beta1
            << " beta2 = " << beta2 << " eps = " << eps << std::endl;
    }

    return uwot::Adam(alpha, beta1, beta2, eps, head_embedding.size());
  }

  auto create_sgd(List opt_args) -> uwot::Sgd {
    float alpha = lget(opt_args, "alpha", 1.0);
    if (verbose) {
      Rcerr << "Optimizing with SGD"
            << " alpha = " << alpha << std::endl;
    }

    return uwot::Sgd(alpha);
  }

  template <typename RandFactory, bool DoMove, typename Gradient>
  void create_impl(const Gradient &gradient, bool batch) {
    if (batch) {
      std::string opt_name = opt_args["method"];
      if (opt_name == "adam") {
        auto opt = create_adam(opt_args);
        create_impl_batch_opt<decltype(opt), RandFactory, DoMove, Gradient>(
            gradient, opt, batch);
      } else if (opt_name == "sgd") {
        auto opt = create_sgd(opt_args);
        create_impl_batch_opt<decltype(opt), RandFactory, DoMove, Gradient>(
            gradient, opt, batch);
      } else {
        stop("Unknown optimization method");
      }
    } else {
      const std::size_t ndim = head_embedding.size() / n_head_vertices;
      uwot::Sampler sampler(epochs_per_sample, negative_sample_rate);
      uwot::InPlaceUpdate<DoMove> update(head_embedding, tail_embedding,
                                         initial_alpha, epoch_callback);
      uwot::EdgeWorker<Gradient, decltype(update), RandFactory> worker(
          gradient, update, positive_head, positive_tail, sampler, ndim,
          n_tail_vertices, n_threads);
      create_impl(worker, gradient);
    }
  }

  template <typename Opt, typename RandFactory, bool DoMove, typename Gradient>
  void create_impl_batch_opt(const Gradient &gradient, Opt &opt, bool batch) {
    uwot::Sampler sampler(epochs_per_sample, negative_sample_rate);
    const std::size_t ndim = head_embedding.size() / n_head_vertices;
    uwot::BatchUpdate<DoMove, decltype(opt)> update(
        head_embedding, tail_embedding, opt, epoch_callback);
    uwot::NodeWorker<Gradient, decltype(update), RandFactory> worker(
        gradient, update, positive_head, positive_tail, positive_ptr, sampler,
        ndim, n_tail_vertices);
    create_impl(worker, gradient);
  }

  template <typename Worker, typename Gradient>
  void create_impl(Worker &worker, const Gradient &gradient) {

    RProgress progress(n_epochs, verbose);
    if (n_threads > 0) {
      RParallel parallel(n_threads, grain_size);
      create_impl(worker, gradient, progress, parallel);
    } else {
      RSerial serial;
      create_impl(worker, gradient, progress, serial);
    }
  }

  template <typename Worker, typename Gradient, typename Progress,
            typename Parallel>
  void create_impl(Worker &worker, const Gradient &gradient, Progress &progress,
                   Parallel &parallel) {
    uwot::optimize_layout(worker, progress, n_epochs, parallel);
  }
};

auto r_to_coords(NumericMatrix head_embedding,
                 Nullable<NumericMatrix> tail_embedding) -> uwot::Coords {
  auto head_vec = as<std::vector<float>>(head_embedding);
  if (tail_embedding.isNull()) {
    return uwot::Coords(head_vec);
  } else {
    auto tail_vec = as<std::vector<float>>(tail_embedding);
    return uwot::Coords(head_vec, tail_vec);
  }
}

auto r_to_coords(NumericMatrix head_embedding) -> uwot::Coords {
  auto head_vec = as<std::vector<float>>(head_embedding);
  return uwot::Coords(head_vec);
}

void validate_args(List method_args,
                   const std::vector<std::string> &arg_names) {
  for (auto &arg_name : arg_names) {
    if (!method_args.containsElementNamed(arg_name.c_str())) {
      stop("Missing embedding method argument: " + arg_name);
    }
  }
}

void create_umap(UmapFactory &umap_factory, List method_args) {
  std::vector<std::string> arg_names = {"a", "b", "gamma", "approx_pow"};
  validate_args(method_args, arg_names);

  float a = method_args["a"];
  float b = method_args["b"];
  float gamma = method_args["gamma"];
  bool approx_pow = method_args["approx_pow"];
  if (approx_pow) {
    const uwot::apumap_gradient gradient(a, b, gamma);
    umap_factory.create(gradient);
  } else {
    const uwot::umap_gradient gradient(a, b, gamma);
    umap_factory.create(gradient);
  }
}

void create_tumap(UmapFactory &umap_factory, List) {
  const uwot::tumap_gradient gradient;
  umap_factory.create(gradient);
}

void create_umapai(UmapFactory &umap_factory, List method_args) {
  std::vector<std::string> arg_names = {"ai", "b", "ndim"};
  validate_args(method_args, arg_names);

  std::vector<float> ai = method_args["ai"];
  float b = method_args["b"];
  std::size_t ndim = method_args["ndim"];
  const uwot::umapai_gradient gradient(ai, b, ndim);
  umap_factory.create(gradient);
}

void create_umapai2(UmapFactory &umap_factory, List method_args) {
  std::vector<std::string> arg_names = {"ai", "aj", "b", "ndim"};
  validate_args(method_args, arg_names);

  std::vector<float> ai = method_args["ai"];
  std::vector<float> aj = method_args["aj"];
  float b = method_args["b"];
  std::size_t ndim = method_args["ndim"];
  const uwot::umapai2_gradient gradient(ai, aj, b, ndim);
  umap_factory.create(gradient);
}

void create_pacmap(UmapFactory &umap_factory, List method_args) {
  std::vector<std::string> arg_names = {"a", "b"};
  validate_args(method_args, arg_names);

  float a = method_args["a"];
  float b = method_args["b"];
  const uwot::pacmap_gradient gradient(a, b);
  umap_factory.create(gradient);
}

void create_largevis(UmapFactory &umap_factory, List method_args) {
  std::vector<std::string> arg_names = {"gamma"};
  validate_args(method_args, arg_names);

  float gamma = method_args["gamma"];
  const uwot::largevis_gradient gradient(gamma);
  umap_factory.create(gradient);
}

// Wrap Rcpp Function for use as a callback
template <bool DoMoveVertex> struct REpochCallback : uwot::EpochCallback {
  Function f;
  std::size_t ndim;
  REpochCallback(Function f, std::size_t ndim) : f(f), ndim(ndim) {}
  void operator()(std::size_t epoch, std::size_t n_epochs,
                  const std::vector<float> &head_embedding,
                  const std::vector<float> &tail_embedding) override {
    NumericMatrix head_mat(ndim, head_embedding.size() / ndim,
                           head_embedding.begin());
    auto head_matt = transpose(head_mat);
    NumericMatrix tail_mat(ndim, tail_embedding.size() / ndim,
                           tail_embedding.begin());
    auto tail_matt = transpose(tail_mat);
    f(epoch + 1, n_epochs, head_matt, tail_matt);
  }
};

template <> struct REpochCallback<true> : uwot::EpochCallback {
  Function f;
  std::size_t ndim;
  REpochCallback(Function f, std::size_t ndim) : f(f), ndim(ndim) {}
  void operator()(std::size_t epoch, std::size_t n_epochs,
                  const std::vector<float> &head_embedding,
                  const std::vector<float> &) override {
    NumericMatrix m(ndim, head_embedding.size() / ndim, head_embedding.begin());
    auto mt = transpose(m);
    f(epoch + 1, n_epochs, mt);
  }
};

auto create_callback(Nullable<Function> epoch_callback, std::size_t ndim,
                     bool move_other) -> uwot::EpochCallback * {
  if (epoch_callback.isNull()) {
    return new uwot::DoNothingCallback();
  } else {
    if (move_other) {
      return new REpochCallback<true>(as<Function>(epoch_callback), ndim);
    } else {
      return new REpochCallback<false>(as<Function>(epoch_callback), ndim);
    }
  }
}

// [[Rcpp::export]]
NumericMatrix optimize_layout_r(
    NumericMatrix head_embedding, Nullable<NumericMatrix> tail_embedding,
    const std::vector<unsigned int> positive_head,
    const std::vector<unsigned int> positive_tail,
    const std::vector<unsigned int> positive_ptr, unsigned int n_epochs,
    unsigned int n_head_vertices, unsigned int n_tail_vertices,
    const std::vector<float> epochs_per_sample, const std::string &method,
    List method_args, float initial_alpha, List opt_args,
    Nullable<Function> epoch_callback, float negative_sample_rate,
    bool pcg_rand = true, bool batch = false, std::size_t n_threads = 0,
    std::size_t grain_size = 1, bool move_other = true, bool verbose = false) {

  auto coords = r_to_coords(head_embedding, tail_embedding);
  const std::size_t ndim = head_embedding.size() / n_head_vertices;
  uwot::EpochCallback *uwot_ecb =
      create_callback(epoch_callback, ndim, move_other);

  UmapFactory umap_factory(move_other, pcg_rand, coords.get_head_embedding(),
                           coords.get_tail_embedding(), positive_head,
                           positive_tail, positive_ptr, n_epochs,
                           n_head_vertices, n_tail_vertices, epochs_per_sample,
                           initial_alpha, opt_args, negative_sample_rate, batch,
                           n_threads, grain_size, uwot_ecb, verbose);
  if (verbose) {
    Rcerr << "Using method '" << method << "'" << std::endl;
  }
  if (method == "umap") {
    create_umap(umap_factory, method_args);
  } else if (method == "tumap") {
    create_tumap(umap_factory, method_args);
  } else if (method == "largevis") {
    create_largevis(umap_factory, method_args);
  } else if (method == "pacmap") {
    create_pacmap(umap_factory, method_args);
  } else if (method == "leopold") {
    create_umapai(umap_factory, method_args);
  } else if (method == "leopold2") {
    create_umapai2(umap_factory, method_args);
  } else {
    stop("Unknown method: '" + method + "'");
  }

  return NumericMatrix(head_embedding.nrow(), head_embedding.ncol(),
                       coords.get_head_embedding().begin());
}
