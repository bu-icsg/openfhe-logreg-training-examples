//==================================================================================
// BSD 2-Clause License
//
// Copyright (c) 2023, Duality Technologies Inc.
//
// All rights reserved.
//
// Author TPOC: contact@openfhe.org
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//==================================================================================

//#define PROFILE

/* Please comment/uncomment these as you see fit:
 * ENABLE_INFO will display informational output during the run
 * ENABLE_DEBUG will display debug info, like actual matrix values
 */
//#define ENABLE_DEBUG

#include "openfhe.h"
#include <iostream>
#include "data_io.h"
#include "lr_train_funcs.h"
#include "lr_types.h"
#include "utils.h"
#include "parameters.h"
#include "minimax.hpp"
#include "PlateauLRScheduler.hpp"
/////////////////////////////////////////////////////////
// Global Values
/////////////////////////////////////////////////////////
int CHEBYSHEV_ESTIMATION_DEGREE(2);
int CHEBYSHEV_RANGE_ESTIMATION_START(-16);
int CHEBYSHEV_RANGE_ESTIMATION_END(16);
usint NUM_ITERS_DEF(200);
usint WRITE_EVERY(10);
bool WITH_BT_DEF(true);
int ROWS_TO_READ_DEF(-1);   //Note this is to verify zero padding
std::string TRAIN_X_FILE_DEF = "../train_data/X_norm_1024.csv";
std::string TRAIN_Y_FILE_DEF = "../train_data/y_1024.csv";
std::string TEST_X_FILE_DEF = "../train_data/X_norm.csv";
std::string TEST_Y_FILE_DEF = "../train_data/y.csv";
uint32_t RING_DIM_DEF(1 << 17);
float LR_GAMMA(0.1);  // Learning Rate
float LR_ETA(0.1);   // Learning Rate

// Note: the ranges were chosen based on empirical observations.
//    Depending on your application, the estimation ranges may change.
//    and the estimation degree should change accordingly. Refer to the
//    following: to understand what degree might be necessary and how the
//    multipication depth requirements will change
// https://github.com/openfheorg/openfhe-development/blob/main/src/pke/examples/FUNCTION_EVALUATION.md#how-to-choose-multiplicative-depth
// int CHEBYSHEV_RANGE_ESTIMATION_START = -16;
// int CHEBYSHEV_RANGE_ESTIMATION_END = 16;
// int CHEBYSHEV_ESTIMATION_DEGREE = 59;
bool DEBUG = true;
int DEBUG_PLAINTEXT_LENGTH = 32;

// If we are in the 64-bit case, we may want to run bootstrapping twice
//    As this will increase our precision, which will make our results
//    more in-line with the 128-bit version.
//    Note: we default this to 0. If, at call-time, the precision is 0
//    we run single-bootstrapping.
int BOOTSTRAP_PRECISION_DEF(0);

void debugWeights(
        CC cc, lbcrypto::KeyPair<lbcrypto::DCRTPoly> keys, const CT& ctWeights,
        const PT& ptExtractThetaMask,
        const PT& ptExtractPhiMask,
        int signedRowSize,
        int slotsBoot
) {
    std::cout << "\tIn DebugWeights function - Separating Theta and Phi" << std::endl;
    CT _ctTheta_DBG = cc->EvalMult(ctWeights, ptExtractThetaMask);
    CT ctTheta_DBG = cc->EvalAdd(
            cc->EvalRotate(_ctTheta_DBG, signedRowSize),
            _ctTheta_DBG
    );
    CT _ctPhi_DBG = cc->EvalMult(ctWeights, ptExtractPhiMask);
    CT ctPhi_DBG = cc->EvalAdd(
            cc->EvalRotate(_ctPhi_DBG, -signedRowSize),
            _ctPhi_DBG
    );

    PT ptTheta;
    PT ptPhi;
    cc->Decrypt(keys.secretKey, ctTheta_DBG, &ptTheta);
    cc->Decrypt(keys.secretKey, ctPhi_DBG, &ptPhi);
    ptTheta->SetLength(slotsBoot);
    ptPhi->SetLength(slotsBoot);

    std::cout << "\t\tTHETA: " << ptTheta << std::endl;
    std::cout << "\t\tPHI: " << ptPhi << std::endl;

    std::cout << "\tExiting DebugWeights function" << std::endl;
}

int main(int argc, char *argv[]) {

    std::cout << GetOPENFHEVersion() << std::endl;

    OPENFHE_DEBUG_FLAG(false);
    // Parse arguments
    OPENFHE_DEBUG(5);

    /////////////////////////////////////////////////////////
    // Setting the default values for everything
    /////////////////////////////////////////////////////////

    Parameters params{};
    params.populateParams(argc, argv, NUM_ITERS_DEF, WITH_BT_DEF, ROWS_TO_READ_DEF,
                          TRAIN_X_FILE_DEF, TRAIN_Y_FILE_DEF, TEST_X_FILE_DEF, TEST_Y_FILE_DEF,
                          RING_DIM_DEF, CHEBYSHEV_ESTIMATION_DEGREE, CHEBYSHEV_RANGE_ESTIMATION_END,
                          CHEBYSHEV_RANGE_ESTIMATION_START, WRITE_EVERY, BOOTSTRAP_PRECISION_DEF, false
    );

    /////////////////////////////////////////////////////////
    // Handle IO for writing
    /////////////////////////////////////////////////////////
    std::ofstream ofsloss;
    ofsloss.precision(params.outputPrecision);
    ofsloss.open(params.lossOutFile);
    if (!ofsloss.is_open()) {
        std::cerr << "Could not open file to write train loss to " << params.lossOutFile << std::endl;
        exit(EXIT_FAILURE);
    }
    ofsloss << "Time Taken(s), " << "Train Losses" << std::endl;

    std::ofstream weightOFS;
    weightOFS.precision(params.outputPrecision);
    weightOFS.open(params.weightsOutFile, std::ofstream::out | std::ofstream::trunc);

    if (!weightOFS.is_open()) {
        std::cerr << "Couldn't open file to write weights to";
        exit(EXIT_FAILURE);
    }
    weightOFS << "Weights" << std::endl;

    std::ofstream testOFS;
    testOFS.precision(params.outputPrecision);
    testOFS.open(params.testLossOutFile, std::ofstream::out | std::ofstream::trunc);
    if (!testOFS.is_open()) {
        std::cerr << "Couldn't open file to write test loss to";
        exit(EXIT_FAILURE);
    }
    testOFS << "Test Losses" << std::endl;


    /////////////////////////////////////////////////////////
    // Crypto CryptoParams
    /////////////////////////////////////////////////////////
    lbcrypto::SecurityLevel securityLevel = lbcrypto::HEStd_128_classic;
//  lbcrypto::SecurityLevel securityLevel = lbcrypto::HEStd_NotSet;
    uint32_t numLargeDigits = 0;
    uint32_t maxRelinSkDeg = 1;

#if NATIVEINT == 128
    std::cout << "Running in 128-bit mode" << std::endl;
  uint32_t firstModSize = 89;
  uint32_t dcrtBits = 78;
#else
    std::cout << "Running in 64-bit mode" << std::endl;
    uint32_t firstModSize = 60;
    uint32_t dcrtBits = 59;
#endif
    uint32_t batchSize = params.ringDimension / 2;
    lbcrypto::ScalingTechnique rsTech = lbcrypto::FIXEDAUTO;
    lbcrypto::KeySwitchTechnique ksTech = lbcrypto::HYBRID;

    CryptoParams parameters;
    std::vector<uint32_t> levelBudget;
    std::vector<uint32_t> bsgsDim = {0, 0};
    uint32_t multDepth;

    if (params.withBT) {
        std::cout << "Using Bootstrapping" << std::endl;
        // Params here set based on discussion in
        // https://github.com/openfheorg/openfhe-development/blob/main/src/pke/examples/advanced-ckks-bootstrapping.cpp
        lbcrypto::SecretKeyDist skDist = lbcrypto::UNIFORM_TERNARY;
        // linear transform using 1 level is good for CKKS bootstrapping as the number of features is small (10)
        levelBudget = {1, 1};
        uint32_t levelsBeforeBootstrap = 27;
        uint32_t approxBootstrapDepth = 8;

#if NATIVEINT == 64
        // Add an extra level based on empirical run results. We've encountered an error of
        //"DCRTPolyImpl's towers are not initialized" and this addition solves that.
        levelsBeforeBootstrap++;
#endif

        multDepth = levelsBeforeBootstrap + lbcrypto::FHECKKSRNS::GetBootstrapDepth(
                approxBootstrapDepth, levelBudget, skDist
        );

        std::cout << "*********************************************" << std::endl;
        std::cout << "Bootstrapping Crypto Params" << std::endl;
        std::cout << "\tDiscrete key used: " << skDist << std::endl;
        std::cout << "\tApprox Bootstrap depth: " << approxBootstrapDepth << std::endl;
        std::cout << "\tLevels before bootstrap: " << levelsBeforeBootstrap << std::endl;

        std::cout << "\tFinal Bootstrap Depth:  " << multDepth << std::endl;
        parameters.SetSecretKeyDist(skDist);
    } else {
        std::cout << "Using Interactive Methods" << std::endl;
        // Unpacking the two ciphertexts

        // EncLogRegCalculateGradient consumes 12 levels:
        // MatrixVectorProductRow takes 2 levels,
        // EvalLogistic for deg = 128 takes 9 levels
        // and then MatrixVectorProductCol takes 1 level

        // then 2 more levels are used after logreg iteration

        // Used to be 12 based on above, but in the case where we pack the
        // weights nto a single ciphertext we need to do extra mults
        // NOTE: Recreating the theta and phi from the single ciphertext
        //      + 1 multiplication and 1 rotation
        // NOTE: Joining theta and phi into a single ciphertext
        //      + 1 multiplication and 1 addition
        multDepth = 13;
    }

    /////////////////////////////////////////////////////////
    // Set crypto params and create context
    /////////////////////////////////////////////////////////
    parameters.SetMultiplicativeDepth(multDepth);
    parameters.SetScalingModSize(dcrtBits);
    parameters.SetBatchSize(batchSize);
    parameters.SetSecurityLevel(securityLevel);
    parameters.SetRingDim(params.ringDimension);
    parameters.SetScalingTechnique(rsTech);
    parameters.SetKeySwitchTechnique(ksTech);
    parameters.SetNumLargeDigits(numLargeDigits);
    parameters.SetFirstModSize(firstModSize);
    parameters.SetMaxRelinSkDeg(maxRelinSkDeg);

    CC cc;
    cc = GenCryptoContext(parameters);

    // Enable the features that you wish to use.
    cc->Enable(lbcrypto::PKE);
    cc->Enable(lbcrypto::LEVELEDSHE);
    cc->Enable(lbcrypto::ADVANCEDSHE);

    if (!cc) {
        std::cout << "Error generating CKKS context... " << std::endl;
        exit(EXIT_FAILURE);
    }
    std::cout << "Generating keys" << std::endl;
    lbcrypto::KeyPair<lbcrypto::DCRTPoly> keys = cc->KeyGen();
    std::cout << "\tMult keys" << std::endl;
    cc->EvalMultKeyGen(keys.secretKey);
    std::cout << "\tEvalSum keys" << std::endl;
    cc->EvalSumKeyGen(keys.secretKey);

    usint numSlots = cc->GetEncodingParams()->GetBatchSize();

    /////////////////////////////////////////////////////////////////
    //Load Plaintext Data
    /////////////////////////////////////////////////////////////////
    Mat NegXt;
    Mat beta;
    Mat X;
    Mat y;
    Mat testX;
    Mat testY;

    PT ptExtractThetaMask;
    PT ptExtractPhiMask;

    populateData(params, cc, keys, NegXt,
                 beta, X, y, testX, testY,
                 ptExtractThetaMask, ptExtractPhiMask, LR_GAMMA
    );

    usint originalNumSamp = X.size();     //n_samp

    usint originalNumFeat = X[0].size();  //n_feat (including the intecept column
    auto dims = ComputePaddedDimensions(originalNumSamp, originalNumFeat, numSlots);
    usint rowSize = dims.second;
    int signedRowSize = (int) rowSize;

    MatKeys evalSumRowKeys = cc->EvalSumRowsKeyGen(keys.secretKey, nullptr, rowSize);
    MatKeys evalSumColKeys = cc->EvalSumColsKeyGen(keys.secretKey);
    /////////////////////////////////////////////////////////////////
    //Encrypt Data
    /////////////////////////////////////////////////////////////////

    CT ctWeights = collateOneDMats2CtVRC(cc, beta, beta, rowSize, numSlots, keys);
    // returns negative X' matrix n_samp x n_features and initializes beta
    auto ctNegXt = Mat2CtMRM(cc, NegXt, rowSize, numSlots, keys);

    ///note these functions WILL zero pad out the matricies
    auto ctX = Mat2CtMRM(cc, X, rowSize, numSlots, keys); //verified ok
    // using mcm because NegXt is -X being transposed by packing.
    CT ctyVCC = OneDMat2CtVCC(cc, y, rowSize, numSlots, keys);
    /////////////////////////////////////////////////////////////////
    //Tracking and debugging
    /////////////////////////////////////////////////////////////////
    PT ptTheta; //plaintext for the resulting beta output
    CT ctGradient;
    double totalTime = 0;
    Vec final_b_vec;
    Mat final_b;

    TimeVar t;
    /////////////////////////////////////////////////////////////////
    // Optimization: set the number of slots for sparse bootstrap
    /////////////////////////////////////////////////////////////////

    auto numFeaturesEnc = NextPow2(originalNumFeat);
    auto numSlotsBoot = numFeaturesEnc * 8;
    if (params.withBT) {
        cc->Enable(lbcrypto::FHE);
        cc->EvalBootstrapSetup(levelBudget, bsgsDim, numSlotsBoot);
        cc->EvalBootstrapKeyGen(keys.secretKey, numSlotsBoot);
    }

    CT ctM, ctMPrime;
    CT ctV, ctVPrime;
    CT ctGrad;

    int bs_moments_degree;
    switch (params.CHEBYSHEV_ESTIMATION_DEGREE) {
        case 2:
            bs_moments_degree = 9;
            break;
        case 5:
            bs_moments_degree = 8;
            break;
        case 13:
            bs_moments_degree = 7;
            break;
        case 27:
            bs_moments_degree = 6;
            break;
        case 59:
            bs_moments_degree = 5;
            break;
        default:
            bs_moments_degree = 1; 
            break;
    }

    vector<double> train_losses(params.numIters);
    // Initialize a plateau scheduler that reduces LR if loss doesn't improve
    int patience = 3; // epochs
    double factor = 0.5;
    PlateauLRScheduler scheduler(LR_ETA, factor, patience, "min");


    /////////////////////////////////////////////////////////////////
    // Logistic regression training loop on encrypted data
    auto mode = (params.withBT) ? "Bootstrap " : "Interactive ";
    std::cout << "Training mode: " << mode << std::endl;
    std::cout << "Bs moments every " << bs_moments_degree << " epochs" << std::endl;
    for (usint epochI = 0; epochI < params.numIters; epochI++) {
        TIC(t);
        std::cout << "Iteration: " << epochI
                  << " ******************************************************************"
                  << std::endl;
//        auto epochInferenceStart = std::chrono::high_resolution_clock::now();
        if ((params.withBT) && epochI > 0) {
            ctWeights->SetSlots(numSlotsBoot);
#if NATIVEINT == 128
            ctWeights = cc->EvalBootstrap(ctWeights);
#else
            // If we are in the 64-bit case, we may want to run bootstrapping twice
            //    As this will increase our precision, which will make our results
            //    more in-line with the 128-bit version

             if ((epochI+1) % bs_moments_degree == 0){
                 std::cout << "Bootstrapping the moments: " << std::endl;
                 ctM->SetSlots(numSlotsBoot);
                 ctV->SetSlots(numSlotsBoot);
                 ctM = cc->EvalBootstrap(ctM);
                 ctV = cc->EvalBootstrap(ctV);
             }

//            std::cout << std::endl << "\tNum levels, 1: " << ctWeights->GetLevel() << std::endl;
            if (params.btPrecision > 0){
                std::cout << "Running double-bootstrapping at: " << params.btPrecision << " precision" << std::endl;
                ctWeights = cc->EvalBootstrap(ctWeights, 2, params.btPrecision);
            } else {
                ctWeights = cc->EvalBootstrap(ctWeights);
            }
//            std::cout << std::endl << "\tNum levels, 2: " << ctWeights->GetLevel() << std::endl;

            std::cout << "\tNum limbs, m: " << ctM->GetElements()[0].GetNumOfElements() << ", v: " << ctV->GetElements()[0].GetNumOfElements() << std::endl;

#endif
            OPENFHE_DEBUGEXP(ctWeights->GetLevel());
        } else {
            OPENFHE_DEBUGEXP(ReturnDepth(ctWeights));
            ReEncrypt(cc, ctWeights, keys);
            OPENFHE_DEBUGEXP(ReturnDepth(ctWeights));
        }

        /////////////////////////////////////////////////////////////////
        // Extract the weights
        //  1) mask out the phi to get just Theta
        //  2) mask
        /////////////////////////////////////////////////////////////////
        CT _ctTheta = cc->EvalMult(ctWeights, ptExtractThetaMask);
        // _ctTheta
        //      - numFeaturesEnc of 0s, numFeaturesEnc of thetas repeating to fill in the entire CT
        // | 0, 0, ..., 0, theta_0, theta_1, ..., theta_15, 0,| (repeated)
        CT ctTheta = cc->EvalAdd(
                cc->EvalRotate(_ctTheta, signedRowSize),  // | 0, theta, 0, theta ...|
                _ctTheta);
        // ctTheta
        // | theta_0, theta_1, ..., theta_15, theta_0, theta_1, ..., theta_15|
        OPENFHE_DEBUGEXP(ctTheta);

        CT _ctPhi = cc->EvalMult(ctWeights, ptExtractPhiMask); // | 0, phi, 0, phi, ...|
        // _ctPhi
        //      - numFeaturesEnc of phis, numFeaturesEnc of 0s repeating to fill in the entire CT
        // | phi_0, phi_1, ..., phi_15, 0, 0, ..., 0|
        CT ctPhi = cc->EvalAdd(
                cc->EvalRotate(_ctPhi, -signedRowSize),
                _ctPhi
        );
        // ctPhi
        // | phi_0, phi_1, ..., phi_15, phi_0, phi_1, ..., phi_15|

#ifdef ENABLE_DEBUG
        OPENFHE_DEBUG("Decrypting the ciphertexts to inspect the values");
    PT ptThetaDBG;
    cc->Decrypt(ctTheta, keys.secretKey, &ptThetaDBG);
    ptTheta->SetLength(signedRowSize * 4);
    OPENFHE_DEBUG(ptThetaDBG);
    for (auto &v : ptThetaDBG->GetCKKSPackedValue()) {
      std::cout << v << ", " << std::endl;
    }

    PT ptPhiDBG;
    cc->Decrypt(ctTheta, keys.secretKey, &ptPhiDBG);
    ptPhiDBG->SetLength(signedRowSize * 4);
    OPENFHE_DEBUG(ptPhiDBG);
    for (auto &v : ptPhiDBG->GetCKKSPackedValue()) {
      std::cout << v << ", " << std::endl;
    }
#endif
        /////////////////////////////////////////////////////////////////
        //Note: Formulation based on:
        //  https://eprint.iacr.org/2018/462.pdf, Algorithm 1
        // and https://jlmelville.github.io/mize/nesterov.html
        /////////////////////////////////////////////////////////////////

        EncLogRegCalculateGradient(cc, ctX, ctNegXt, ctyVCC, ctTheta, ctGradient,
                                   rowSize, evalSumRowKeys, evalSumColKeys, keys,
                                   false,
                                   params.CHEBYSHEV_RANGE_ESTIMATION_START,
                                   params.CHEBYSHEV_RANGE_ESTIMATION_END,
                                   params.CHEBYSHEV_ESTIMATION_DEGREE,
                                   DEBUG_PLAINTEXT_LENGTH
        );
#ifdef ENABLE_DEBUG
        PT ptGrad;
    cc->Decrypt(keys.secretKey, ctGradient, &ptGrad);
    std::cout << "\tGradient: " << ptGrad << std::endl;
#endif
        OPENFHE_DEBUG("Applying gradient");
        /////////////////////////////////////////////////////////////////
        //Note: Formulation of NAG update based on
        // and https://jlmelville.github.io/mize/nesterov.html
        /////////////////////////////////////////////////////////////////

//         auto ctPhiPrime = cc->EvalSub(
//             ctTheta,
//             ctGradient
//         );
//
//         if (epochI == 0) {
//           ctTheta = ctPhiPrime;
//         } else {
//           ctTheta = cc->EvalAdd(
//               ctPhiPrime,
//               cc->EvalMult(
//                   LR_ETA,
//                   cc->EvalSub(ctPhiPrime, ctPhi)
//               )
//           );
//         }
//         ctPhi = ctPhiPrime;

        /////////////////////////////////////////////////////////////////
        // Adam
        /////////////////////////////////////////////////////////////////
        float BETA1 = 0.9;
        float BETA2 = 0.999;

        float beta_correction = sqrt((1 - pow(BETA2, epochI + 1)))/(1 - pow(BETA1, epochI + 1) + (0.000000001));

        ctGrad = ctGradient->Clone();
        PT grad, weights;
        cc->Decrypt(keys.secretKey, ctGradient, &grad);
        grad->SetLength(8);
        cc->Decrypt(keys.secretKey, ctWeights, &weights);
        weights->SetLength(8);

        double current_lr = scheduler.get_lr();

        std::cout << "beta_correction: " << beta_correction << std::endl;
        std::cout << "learning rate: " << current_lr << std::endl;
        std::cout << "Gradient: " << grad;

        if (epochI == 0){
            ctMPrime = cc->EvalMult((1-BETA1), ctGrad);
            ctVPrime = cc->EvalMult((1-BETA2), cc->EvalMult(ctGrad, ctGrad));
        }
        else {
            ctMPrime = cc->EvalAdd(cc->EvalMult(BETA1, ctM),
                                   cc->EvalMult((1-BETA1), ctGrad));
            ctVPrime = cc->EvalAdd(cc->EvalMult(BETA2, ctV),
                                   cc->EvalMult((1-BETA2), cc->EvalMult(ctGrad, ctGrad)));
        }

       auto div_vHat = cc->EvalChebyshevFunction([](double x) -> double { return 1/(sqrt(x)+0.00000000000000001); }, ctVPrime, 0, 1, 59);
        // auto div_vHat = MinimaxEvaluation(f, ctVPrime, 0, 1, 2);
        auto adam_update = cc->EvalMult(cc->EvalMult(beta_correction*current_lr, ctMPrime), div_vHat);
        ctTheta = cc->EvalSub(
                ctTheta,
                adam_update
        );

        PT ptM, ptV;
        cc->Decrypt(keys.secretKey, ctMPrime, &ptM);
        ptM->SetLength(8);
        std::cout << "m: " << ptM;

        cc->Decrypt(keys.secretKey, ctVPrime, &ptV);
        ptV->SetLength(8);
        std::cout << "v: " << ptV;

        PT ptdiv_vHat;
        cc->Decrypt(keys.secretKey, div_vHat, &ptdiv_vHat);
        ptdiv_vHat->SetLength(8);
        std::cout << "1/sqrt(v): " << ptdiv_vHat;

        PT ptupdate;
        cc->Decrypt(keys.secretKey, adam_update, &ptupdate);
        ptupdate->SetLength(8);
        std::cout << "update: " << ptupdate;

        ctM = ctMPrime;
        ctV = ctVPrime; // Store current moment

        // Step 11
        if (DEBUG) {
            cc->Decrypt(keys.secretKey, ctTheta, &ptTheta);

            final_b_vec = ptTheta->GetRealPackedValue();

            final_b_vec.resize(originalNumFeat);

            final_b = Mat(originalNumFeat, Vec(1, 0.0));
            //copy values into final_b matrix
            std::cout << "\tNew weights: ";
            for (auto copyI = 0U; copyI < originalNumFeat; copyI++) {
                std::cout << final_b_vec[copyI] << ",";
                final_b[copyI][0] = final_b_vec[copyI];
            }
            std::cout << std::endl;

            auto loss = ComputeLoss(final_b, X, y);

            train_losses[epochI] = loss;
            scheduler.step(train_losses[epochI]);

            /////////////////////////////////////////////////////////////////
            //Saving and logging information
            /////////////////////////////////////////////////////////////////

            auto epochTime = TOC(t);
            totalTime += epochTime;

            std::cout << "\tLoss: " << loss << "\t took: "
                      << epochTime / 1000.0 << " s" << std::endl;
            OPENFHE_DEBUG(loss);
            ofsloss << epochTime << ", " << loss << std::endl;

            if ((epochI + 1) % WRITE_EVERY == 0 && epochI > 0) {
                std::cout << "\t Writing weights and test loss to files: " << "(" <<
                          params.weightsOutFile << ", " << params.testLossOutFile << ")" << std::endl;
                /////////////////////////////////////////////////////////////////
                // Writing the weights
                /////////////////////////////////////////////////////////////////

                weightOFS << epochI << ",";
                OPENFHE_DEBUG("Writing weights to: " + params.weightsOutFile);
                for (auto &singletonWeight : final_b) {
                    weightOFS << singletonWeight[0] << ",";
                }
                weightOFS << std::endl;
                /////////////////////////////////////////////////////////////////
                // Writing the Test Loss
                /////////////////////////////////////////////////////////////////
                OPENFHE_DEBUG("Writing test loss to: " + params.testLossOutFile);
                auto testLoss = ComputeLoss(final_b, testX, testY);
                std::cout << "\tTest Loss: " << testLoss << std::endl;
                testOFS << epochI << ", " << testLoss << std::endl;
            }
        }
        // training loop ends
        /////////////////////////////////////////////////////////////////

        /////////////////////////////////////////////////////////////////
        // Packing the two ciphertexts back
        /////////////////////////////////////////////////////////////////
        OPENFHE_DEBUG("Repacking the ciphertexts");
        ctTheta = cc->EvalMult(ctTheta, ptExtractThetaMask);  // | theta, 0, theta, 0|
        ctPhi = cc->EvalMult(ctPhi, ptExtractPhiMask);  //| 0, phi, 0, phi|
        ctWeights = cc->EvalAdd(ctTheta, ctPhi);

//        auto epochInferenceEnd = std::chrono::high_resolution_clock::now();
//        auto inferenceDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
//                epochInferenceEnd - epochInferenceStart
//        );
    }
    ofsloss.close();
    weightOFS.close();
    testOFS.close();
    std::cout << "Total Time for training " << params.numIters << " epochs was " << totalTime / 1000.0 << " s"
              << std::endl;
}
