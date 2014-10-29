#include <Eigen/Core>
#include <stdint.h> // int32_t
#include <omp.h> // omp_get_num_threads, omp_get_thread_num
#include <limits> // infinity
#include <iostream> // cout, endl

#include "nptypes.h"
#include "util.h"

using namespace Eigen;
using namespace nptypes;
using namespace std;

template <typename Type>
class dummy
{
    public:

    static void initParallel() {
        Eigen::initParallel();
    }

    static Type resample_arhmm(
            int M, int T, int D, int nlags, bool affine,
            Type *pi_0, Type *A,
            Type *natparams, Type *normalizers,
            Type *data,
            Type *stats, int32_t *counts, int32_t *transcounts, int32_t *stateseq,
            Type *randseq,
            Type *alphan)
    {
        Map<Matrix<Type,Dynamic,Dynamic,RowMajor>,Aligned,OuterStride<>>
            edata(data,T-nlags,D*(nlags+1),OuterStride<>(D));

        NPMatrix<Type> eA(A,M,M);

        int sz = D*(nlags+1) + affine;
        NPMatrix<Type> enatparams(natparams,M*sz,sz);
        NPMatrix<Type> estats(stats,M*sz,sz);
        NPMatrix<int32_t> etranscounts(transcounts,M,M);

        NPMatrix<Type> ealphan(alphan,T-nlags,M);

        Type temp_buf[sz] __attribute__((aligned(16)));
        NPVector<Type> etemp(temp_buf,sz);
        Type data_buff[sz] __attribute__((aligned(16)));
        NPRowVector<Type> data_buf(data_buff,sz);
        Type in_potential_buf[M] __attribute__((aligned(16)));
        NPRowVector<Type> ein_potential(in_potential_buf,M);
        Type likes_buf[M] __attribute__((aligned(16)));
        NPRowVector<Type> elikes(likes_buf,M);
        bool good_data[T-nlags];

        Type norm, cmax, logtot = 0.;
        data_buff[D*nlags] = affine; // extra affine 1.

        // likelihoods and forward messages
        ein_potential = NPMatrix<Type>(pi_0,1,M);
        for (int t=0; t < T-nlags; t++) {
            if (good_data[t] = likely((edata.row(t).array() == edata.row(t).array()).all())) {
                for (int m=0; m < M; m++) {
                    if (affine) {
                        data_buf.segment(0,D*nlags) = edata.row(t).segment(0,D*nlags);
                        data_buf.segment(D*nlags+1,D) = edata.row(t).segment(D*nlags,D);
                    } else {
                        data_buf = edata.row(t);
                    }
                    etemp.noalias() =
                        enatparams.block(m*sz,0,sz,sz)
                        * data_buf.transpose();
                        //.template selfadjointView<Lower>() * data_buf.transpose(); // slower
                    elikes(m) = etemp.dot(data_buf.transpose()) - normalizers[m];
                }

                cmax = elikes.maxCoeff();
                ealphan.row(t) = ein_potential.array() * (elikes.array() - cmax).exp();
                norm = ealphan.row(t).sum();
                ealphan.row(t) /= norm;
                logtot += log(norm) + cmax;
            } else {
                ealphan.row(t) = ein_potential;
            }

            ein_potential = ealphan.row(t) * eA;
        }

        // backward sampling and stats gathering
        ein_potential.setOnes();
        int next_state = 0;
        for (int t=T-nlags-1; t >= 0; t--) {
            elikes = ein_potential.array() * ealphan.row(t).array();
            stateseq[t] = util::sample_discrete<Type>(M,elikes.data(),randseq[t]);
            etranscounts(stateseq[t],next_state) += 1;

            ein_potential = eA.col(stateseq[t]).transpose();
            next_state = stateseq[t];

            if (good_data[t]) {
                counts[stateseq[t]] += 1;

                if (affine) {
                    data_buf.segment(0,D*nlags) = edata.row(t).segment(0,D*nlags);
                    data_buf.segment(D*nlags+1,D) = edata.row(t).segment(D*nlags,D);
                } else {
                    data_buf = edata.row(t);
                }
                // estats.block(stateseq[t]*sz,0,sz,sz).noalias()
                //     += data_buf.transpose() * data_buf;
                estats.block(stateseq[t]*sz,0,sz,sz)
                    .template selfadjointView<Lower>().rankUpdate(data_buf.transpose(),1.);
            }
        }

        // undo the extra trans count
        etranscounts(stateseq[T-nlags-1],0) -= 1;

        // symmetrize statistics (asymmetric storage due to rankUpdate)
        for (int m=0; m < M; m++) {
            estats.block(m*sz,0,sz,sz).template triangularView<Upper>()
                = estats.block(m*sz,0,sz,sz).transpose();
        }

        return logtot;
    }


    static Type resample_inb_arhsmm(
            int M, int T, int D, int nlags, int delay, bool affine,
            int *rs, Type *ps,
            // TODO pass in enters
            Type *pi_0, Type *A,
            Type *natparams, Type *normalizers,
            Type *data,
            Type *stats, int32_t *counts, int32_t *transcounts, int32_t *stateseq,
            Type *randseq,
            Type *alphan, int alphan_sz)
    {
        Map<Matrix<Type,Dynamic,Dynamic,RowMajor>,Aligned,OuterStride<>>
            edata(data,T-nlags,D*(nlags+1),OuterStride<>(D));

        NPMatrix<Type> eA(A,M,M);

        int sz = D*(nlags+1) + affine;
        NPMatrix<Type> enatparams(natparams,M*sz,sz);
        NPMatrix<Type> estats(stats,M*sz,sz);
        NPMatrix<int32_t> etranscounts(transcounts,M,M);

        NPMatrix<Type> ealphan(alphan,T-nlags,alphan_sz);

        Type temp_buf[sz] __attribute__((aligned(16)));
        NPVector<Type> etemp(temp_buf,sz);
        Type data_buff[sz] __attribute__((aligned(16)));
        NPRowVector<Type> data_buf(data_buff,sz);
        Type in_potential_buf[alphan_sz] __attribute__((aligned(16)));
        NPRowVector<Type> ein_potential(in_potential_buf,alphan_sz);
        Type likes_buf[alphan_sz] __attribute__((aligned(16)));
        NPRowVector<Type> elikes(likes_buf,alphan_sz);
        bool good_data[T-nlags];

        Type norm, cmax, logtot = 0.;
        data_buff[D*nlags] = affine; // extra affine 1.

        // likelihoods and forward messages
        ein_potential = NPMatrix<Type>(pi_0,1,alphan_sz);
        for (int t=0; t < T-nlags; t++) {
            if (good_data[t] = likely((edata.row(t).array() == edata.row(t).array()).all())) {
                for (int m=0; m < M; m++) {
                    if (affine) {
                        data_buf.segment(0,D*nlags) = edata.row(t).segment(0,D*nlags);
                        data_buf.segment(D*nlags+1,D) = edata.row(t).segment(D*nlags,D);
                    } else {
                        data_buf = edata.row(t);
                    }
                    etemp.noalias() =
                        enatparams.block(m*sz,0,sz,sz)
                        * data_buf.transpose();
                        //.template selfadjointView<Lower>() * data_buf.transpose(); // slower
                    elikes(m) = etemp.dot(data_buf.transpose()) - normalizers[m];
                }

                // ealphan.row(t) = ein_potential.array() * (elikes.array() - cmax).exp();
                cmax = elikes.maxCoeff();
                for (int m=0, idx=0; m < M; idx+=rs[m]+delay, m++) {
                    ealphan.block(t,idx,1,rs[m]+delay).noalias()
                        = ein_potential.segment(idx,rs[m]+delay) * exp(elikes(m) - cmax);
                    idx += rs[m]+delay;
                }

                norm = ealphan.row(t).sum();
                ealphan.row(t) /= norm;
                logtot += log(norm) + cmax;
            } else {
                ealphan.row(t) = ein_potential;
            }

            // TODO
            // ein_potential = ealphan.row(t) * eA;
            for (int m=0, idx=0; m < M; idx+=rs[m]+delay, m++) {
                ein_potential(idx) = 0;
                ein_potential.segment(idx+1,delay-1) = ealphan.row(t).segment(idx,delay-1);
                ein_potential.segment(idx+delay,rs[m]) = ps[m]*ealphan.row(t).segment(idx+delay,rs[m]);
                ein_potential.segment(idx+delay+1,rs[m]-1) += (1-ps[m])*ealphan.row(t).segment(idx+delay,rs[m]-1);
                ein_potential(idx+delay+1) += ealphan(t,idx+delay);
                // TODO copy outs
            }
            // TODO off block diag
        }

        // backward sampling and stats gathering
        ein_potential.setOnes();
        int next_state = 0; // NOTE: extra count in etranscounts
        for (int t=T-nlags-1; t >= 0; t--) {
            elikes = ein_potential.array() * ealphan.row(t).array();
            stateseq[t] = util::sample_discrete<Type>(alphan_sz,elikes.data(),randseq[t]);
            etranscounts(stateseq[t],next_state) += 1;

            // TODO fill in this row better
            ein_potential = eA.col(stateseq[t]).transpose();
            next_state = stateseq[t];

            if (good_data[t]) {
                counts[stateseq[t]] += 1;

                if (affine) {
                    data_buf.segment(0,D*nlags) = edata.row(t).segment(0,D*nlags);
                    data_buf.segment(D*nlags+1,D) = edata.row(t).segment(D*nlags,D);
                } else {
                    data_buf = edata.row(t);
                }
                // estats.block(stateseq[t]*sz,0,sz,sz).noalias()
                //     += data_buf.transpose() * data_buf;
                estats.block(stateseq[t]*sz,0,sz,sz)
                    .template selfadjointView<Lower>().rankUpdate(data_buf.transpose(),1.);
            }
        }

        // undo the extra trans count
        etranscounts(stateseq[T-nlags-1],0) -= 1;

        // symmetrize statistics (asymmetric storage due to rankUpdate)
        for (int m=0; m < M; m++) {
            estats.block(m*sz,0,sz,sz).template triangularView<Upper>()
                = estats.block(m*sz,0,sz,sz).transpose();
        }

        return logtot;
    }
    // TODO later:
    // - save memory by mapping states and only computing the statistics we need
    // - avoid passing in full trans matrix
    // - save memory by computing HSMM messages (?)

};
