/*
 *   Copyright (C) 2015-2020 Mateusz Łącki and Michał Startek.
 *
 *   This file is part of IsoSpec.
 *
 *   IsoSpec is free software: you can redistribute it and/or modify
 *   it under the terms of the Simplified ("2-clause") BSD licence.
 *
 *   IsoSpec is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *   You should have received a copy of the Simplified BSD Licence
 *   along with IsoSpec.  If not, see <https://opensource.org/licenses/BSD-2-Clause>.
 */


#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <utility>
#include <cstring>
#include <string>
#include <limits>
#include <cfenv>
#include "platform.h"
#include "marginalTrek++.h"
#include "conf.h"
#include "allocator.h"
#include "operators.h"
#include "summator.h"
#include "element_tables.h"
#include "misc.h"


namespace IsoSpec
{

//! Find one of the most probable subisotopologues.
/*!
    The algorithm uses the hill-climbing algorithm.
    It starts from a subisotopologue close to the mean of the underlying multinomial distribution.
    There might be more than one modes, in case of which this function will return only one of them, close to the mean.

    \param atomCnt

*/
void writeInitialConfiguration(const int atomCnt, const int isotopeNo, const double* lprobs, int* res)
{
    /*!
    Here we perform hill climbing to the mode of the marginal distribution (the subisotopologue distribution).
    We start from the point close to the mean of the underlying multinomial distribution.
    */

    // This approximates the mode (heuristics: the mean is close to the mode).
    for(int i = 0; i < isotopeNo; ++i)
        res[i] = static_cast<int>( atomCnt * exp(lprobs[i]) ) + 1;

    // The number of assigned atoms above.
    int s = 0;

    for(int i = 0; i < isotopeNo; ++i) s += res[i];

    int diff = atomCnt - s;

    // Too little: enlarging fist index.
    if( diff > 0 ){
        res[0] += diff;
    }
    // Too much: trying to redistribute the difference: hopefully the first element is the largest.
    if( diff < 0 ){
        diff = abs(diff);
        int i = 0;

        while( diff > 0){
            int coordDiff = res[i] - diff;

            if( coordDiff >= 0 ){
                res[i] -= diff;
                diff = 0;
            } else {
                res[i] = 0;
                i++;
                diff = abs(coordDiff);
            }
        }
    }

    // What we computed so far will be very close to the mode: hillclimb the rest of the way

    bool modified = true;
    double LP = unnormalized_logProb(res, lprobs, isotopeNo);
    double NLP;

    while(modified)
    {
        modified = false;
        for(int ii = 0; ii < isotopeNo; ii++)
            for(int jj = 0; jj < isotopeNo; jj++)
                if(ii != jj && res[ii] > 0)
                {
                    res[ii]--;
                    res[jj]++;
                    NLP = unnormalized_logProb(res, lprobs, isotopeNo);
                    if(NLP > LP || (NLP == LP && ii > jj))
                    {
                        modified = true;
                        LP = NLP;
                    }
                    else
                    {
                        res[ii]++;
                        res[jj]--;
                    }
                }
    }
}


double* getMLogProbs(const double* probs, int isoNo)
{
    /*!
    Here we order the processor to round the numbers up rather than down.
    Rounding down could result in the algorithm falling in an infinite loop
    because of the numerical instability of summing.
    */
    for(int ii = 0; ii < isoNo; ii++)
        if(probs[ii] <= 0.0 || probs[ii] > 1.0)
            throw std::invalid_argument("All isotope probabilities p must fulfill: 0.0 < p <= 1.0");

    int curr_method = fegetround();
    fesetround(FE_UPWARD);
    double* ret = new double[isoNo];

    // here we change the table of probabilities and log it.
    for(int i = 0; i < isoNo; i++)
    {
        ret[i] = log(probs[i]);
        for(int j = 0; j < ISOSPEC_NUMBER_OF_ISOTOPIC_ENTRIES; j++)
            if(elem_table_probability[j] == probs[i])
            {
                ret[i] = elem_table_log_probability[j];
                break;
            }
    }
    fesetround(curr_method);
    return ret;
}

double get_loggamma_nominator(int x)
{
    // calculate log gamma of the nominator calculated in the binomial exression.
    int curr_method = fegetround();
    fesetround(FE_UPWARD);
    double ret = lgamma(x+1);
    fesetround(curr_method);
    return ret;
}

int verify_atom_cnt(int atomCnt)
{
    if(ISOSPEC_G_FACT_TABLE_SIZE-1 <= atomCnt)
        throw std::length_error("Subisotopologue too large, size limit (that is, the maximum number of atoms of a single element in a molecule) is: " + std::to_string(ISOSPEC_G_FACT_TABLE_SIZE-1));
    return atomCnt;
}

Marginal::Marginal(
    const double* _masses,
    const double* _probs,
    int _isotopeNo,
    int _atomCnt
) :
disowned(false),
isotopeNo(_isotopeNo),
atomCnt(verify_atom_cnt(_atomCnt)),
atom_lProbs(getMLogProbs(_probs, isotopeNo)),
atom_masses(array_copy<double>(_masses, _isotopeNo)),
loggamma_nominator(get_loggamma_nominator(_atomCnt)),
mode_conf(nullptr)
// Deliberately not initializing mode_lprob
{}

Marginal::Marginal(const Marginal& other) :
disowned(false),
isotopeNo(other.isotopeNo),
atomCnt(other.atomCnt),
atom_lProbs(array_copy<double>(other.atom_lProbs, isotopeNo)),
atom_masses(array_copy<double>(other.atom_masses, isotopeNo)),
loggamma_nominator(other.loggamma_nominator)
{
    if(other.mode_conf == nullptr)
    {
        mode_conf = nullptr;
        // Deliberately not initializing mode_lprob. In this state other.mode_lprob is uninitialized too.
    }
    else
    {
        mode_conf = array_copy<int>(other.mode_conf, isotopeNo);
        mode_lprob = other.mode_lprob;
    }
}


// the move-constructor: used in the specialization of the marginal.
Marginal::Marginal(Marginal&& other) :
disowned(other.disowned),
isotopeNo(other.isotopeNo),
atomCnt(other.atomCnt),
atom_lProbs(other.atom_lProbs),
atom_masses(other.atom_masses),
loggamma_nominator(other.loggamma_nominator)
{
    other.disowned = true;
    if(other.mode_conf == nullptr)
    {
        mode_conf = nullptr;
        // Deliberately not initializing mode_lprob. In this state other.mode_lprob is uninitialized too.
    }
    else
    {
        mode_conf = array_copy<int>(other.mode_conf, isotopeNo);
        mode_lprob = other.mode_lprob;
    }
}

Marginal::~Marginal()
{
    if(!disowned)
    {
        delete[] atom_masses;
        delete[] atom_lProbs;
        delete[] mode_conf;
    }
}


Conf Marginal::computeModeConf() const
{
    Conf res = new int[isotopeNo];
    writeInitialConfiguration(atomCnt, isotopeNo, atom_lProbs, res);
    return res;
}

void Marginal::setupMode()
{
    ISOSPEC_IMPOSSIBLE(mode_conf != nullptr);
    mode_conf = computeModeConf();
    mode_lprob = logProb(mode_conf);
}


double Marginal::getLightestConfMass() const
{
    double ret_mass = std::numeric_limits<double>::infinity();
    for(unsigned int ii = 0; ii < isotopeNo; ii++)
        if( ret_mass > atom_masses[ii] )
            ret_mass = atom_masses[ii];
    return ret_mass*atomCnt;
}

double Marginal::getHeaviestConfMass() const
{
    double ret_mass = 0.0;
    for(unsigned int ii = 0; ii < isotopeNo; ii++)
        if( ret_mass < atom_masses[ii] )
            ret_mass = atom_masses[ii];
    return ret_mass*atomCnt;
}

double Marginal::getMonoisotopicConfMass() const
{
    double found_prob = -std::numeric_limits<double>::infinity();
    double found_mass = 0.0;  // to avoid uninitialized var warning
    for(unsigned int ii = 0; ii < isotopeNo; ii++)
        if( found_prob < atom_lProbs[ii] )
        {
            found_prob = atom_lProbs[ii];
            found_mass = atom_masses[ii];
        }
    return found_mass*atomCnt;
}

double Marginal::getAtomAverageMass() const
{
    double ret = 0.0;
    for(unsigned int ii = 0; ii < isotopeNo; ii++)
        ret += exp(atom_lProbs[ii]) * atom_masses[ii];
    return ret;
}

double Marginal::variance() const
{
    double ret = 0.0;
    double mean = getAtomAverageMass();
    for(size_t ii = 0; ii < isotopeNo; ii++)
    {
        double msq = atom_masses[ii] - mean;
        ret += exp(atom_lProbs[ii]) * msq * msq;
    }
    return ret * atomCnt;
}

double Marginal::getLogSizeEstimate(double logEllipsoidRadius) const
{
    if(isotopeNo <= 1)
        return -std::numeric_limits<double>::infinity();

    const double i = static_cast<double>(isotopeNo);
    const double k = i - 1.0;
    const double n = static_cast<double>(atomCnt);

    double sum_lprobs = 0.0;
    for(int jj = 0; jj < i; jj++)
        sum_lprobs += atom_lProbs[jj];

    double log_V_simplex = k * log(n) - lgamma(i);
    double log_N_simplex = lgamma(n+i) - lgamma(n+1.0) - lgamma(i);
    double log_V_ellipsoid = (k * (log(n) + logpi + logEllipsoidRadius) + sum_lprobs) * 0.5 - lgamma((i+1)*0.5);

    return log_N_simplex + log_V_ellipsoid - log_V_simplex;
}


// this is roughly an equivalent of IsoSpec-Threshold-Generator
MarginalTrek::MarginalTrek(
    Marginal&& m,
    int tabSize,
    int hashSize
) :
Marginal(std::move(m)),
current_count(0),
keyHasher(isotopeNo),
equalizer(isotopeNo),
orderMarginal(atom_lProbs, isotopeNo),
visited(hashSize, keyHasher, equalizer),
pq(orderMarginal),
totalProb(),
candidate(new int[isotopeNo]),
allocator(isotopeNo, tabSize)
{
    int* initialConf = allocator.makeCopy(mode_conf);

    pq.push(initialConf);
    visited[initialConf] = 0;

    totalProb = Summator();

    current_count = 0;

    add_next_conf();
}


bool MarginalTrek::add_next_conf()
{
    /*!
    Add next configuration.
    If visited all, return false.
    */
    if(pq.size() < 1) return false;

    Conf topConf = pq.top();
    pq.pop();
    ++current_count;
    visited[topConf] = current_count;

    _confs.push_back(topConf);
    _conf_masses.push_back(calc_mass(topConf, atom_masses, isotopeNo));
    double logprob = logProb(topConf);
    _conf_lprobs.push_back(logprob);


    totalProb.add( exp( logprob ) );

    for( unsigned int i = 0; i < isotopeNo; ++i )
    {
        for( unsigned int j = 0; j < isotopeNo; ++j )
        {
            // Growing index different than decreasing one AND
            // Remain on simplex condition.
            if( i != j && topConf[j] > 0 ){
                copyConf(topConf, candidate, isotopeNo);

                ++candidate[i];
                --candidate[j];

                // candidate should not have been already visited.
                if( visited.count( candidate ) == 0 )
                {
                    Conf acceptedCandidate = allocator.makeCopy(candidate);
                    pq.push(acceptedCandidate);

                    visited[acceptedCandidate] = 0;
                }
            }
        }
    }

    return true;
}

int MarginalTrek::processUntilCutoff(double cutoff)
{
    Summator s;
    int last_idx = -1;
    for(unsigned int i = 0; i < _conf_lprobs.size(); i++)
    {
        s.add(_conf_lprobs[i]);
        if(s.get() >= cutoff)
        {
            last_idx = i;
            break;
        }
    }
    if(last_idx > -1)
        return last_idx;

    while(totalProb.get() < cutoff && add_next_conf()) {}
    return _conf_lprobs.size();
}


MarginalTrek::~MarginalTrek()
{
    delete[] candidate;
}




PrecalculatedMarginal::PrecalculatedMarginal(Marginal&& m,
    double lCutOff,
    bool sort,
    int tabSize,
    int hashSize
) : Marginal(std::move(m)),
allocator(isotopeNo, tabSize)
{
    const ConfEqual equalizer(isotopeNo);
    const KeyHasher keyHasher(isotopeNo);
    const ConfOrderMarginalDescending orderMarginal(atom_lProbs, isotopeNo);

    std::unordered_set<Conf, KeyHasher, ConfEqual> visited(hashSize, keyHasher, equalizer);

    Conf currentConf = allocator.makeCopy(mode_conf);
    if(logProb(currentConf) >= lCutOff)
    {
        // create a copy and store a ptr to the *same* copy in both structures
        // (save some space and time)
        auto tmp = allocator.makeCopy(currentConf);
        configurations.push_back(tmp);
        visited.insert(tmp);
    }

    unsigned int idx = 0;

    while(idx < configurations.size())
    {
        memcpy(currentConf, configurations[idx], sizeof(int)*isotopeNo);
        idx++;
        for(unsigned int ii = 0; ii < isotopeNo; ii++ )
            for(unsigned int jj = 0; jj < isotopeNo; jj++ )
                if( ii != jj && currentConf[jj] > 0)
                {
                    currentConf[ii]++;
                    currentConf[jj]--;

                    if (visited.count(currentConf) == 0 && logProb(currentConf) >= lCutOff)
                    {
                        // create a copy and store a ptr to the *same* copy in
                        // both structures (save some space and time)
                        auto tmp = allocator.makeCopy(currentConf);
                        visited.insert(tmp);
                        configurations.push_back(tmp);
                        // std::cout << " V: "; for (auto it : visited) std::cout << it << " "; std::cout << std::endl;
                    }

                    currentConf[ii]--;
                    currentConf[jj]++;
                }
    }

    // orderMarginal defines the order of configurations (compares their logprobs)
    // akin to key in Python sort.
    if(sort)
        std::sort(configurations.begin(), configurations.end(), orderMarginal);


    confs  = &configurations[0];
    no_confs = configurations.size();
    lProbs = new double[no_confs+1];
    probs = new double[no_confs];
    masses = new double[no_confs];


    for(unsigned int ii = 0; ii < no_confs; ii++)
    {
        lProbs[ii] = logProb(confs[ii]);
        probs[ii] = exp(lProbs[ii]);
        masses[ii] = calc_mass(confs[ii], atom_masses, isotopeNo);
    }
    lProbs[no_confs] = -std::numeric_limits<double>::infinity();
}


PrecalculatedMarginal::~PrecalculatedMarginal()
{
    if(lProbs != nullptr)
        delete[] lProbs;
    if(masses != nullptr)
        delete[] masses;
    if(probs != nullptr)
        delete[] probs;
}







LayeredMarginal::LayeredMarginal(Marginal&& m, int tabSize, int _hashSize)
: Marginal(std::move(m)), current_threshold(1.0), allocator(isotopeNo, tabSize), sorted_up_to_idx(0),
equalizer(isotopeNo), keyHasher(isotopeNo), orderMarginal(atom_lProbs, isotopeNo), hashSize(_hashSize)
{
    fringe.push_back(mode_conf);
    lProbs.push_back(std::numeric_limits<double>::infinity());
    lProbs.push_back(-std::numeric_limits<double>::infinity());
    guarded_lProbs = lProbs.data()+1;
}

bool LayeredMarginal::extend(double new_threshold)
{
    if(fringe.empty())
        return false;

    // TODO(michalsta): Make sorting optional (controlled by argument?)
    std::vector<Conf> new_fringe;
    std::unordered_set<Conf, KeyHasher, ConfEqual> visited(hashSize, keyHasher, equalizer);

    for(unsigned int ii = 0; ii < fringe.size(); ii++)
        visited.insert(fringe[ii]);

    Conf currentConf;
    while(!fringe.empty())
    {
        currentConf = fringe.back();
        fringe.pop_back();

        double opc = logProb(currentConf);

        if(opc < new_threshold)
            new_fringe.push_back(currentConf);

        else
        {
            configurations.push_back(currentConf);
            for(unsigned int ii = 0; ii < isotopeNo; ii++ )
                for(unsigned int jj = 0; jj < isotopeNo; jj++ )
                    if( ii != jj && currentConf[jj] > 0 )
                    {
                        currentConf[ii]++;
                        currentConf[jj]--;

                        double lpc = logProb(currentConf);

                        if (visited.count(currentConf) == 0 && lpc < current_threshold &&
                            (opc > lpc || (opc == lpc && ii > jj)))
                        {
                            Conf nc = allocator.makeCopy(currentConf);
                            currentConf[ii]--;
                            currentConf[jj]++;
                            visited.insert(nc);
                            if(lpc >= new_threshold)
                                fringe.push_back(nc);
                            else
                                new_fringe.push_back(nc);
                        }
                        else
                        {
                            currentConf[ii]--;
                            currentConf[jj]++;
                        }
                    }
        }
    }

    current_threshold = new_threshold;
    fringe.swap(new_fringe);

    std::sort(configurations.begin()+sorted_up_to_idx, configurations.end(), orderMarginal);

    if(lProbs.capacity() * 2 < configurations.size() + 2)
    {
        // Reserve space for new values
        lProbs.reserve(configurations.size()+2);
        probs.reserve(configurations.size());
        masses.reserve(configurations.size());
    }  // Otherwise we're growing slowly enough that standard reallocations on push_back work better - we waste some extra memory
       // but don't reallocate on every call

    lProbs.pop_back();  // The guardian...

    for(unsigned int ii = sorted_up_to_idx; ii < configurations.size(); ii++)
    {
        lProbs.push_back(logProb(configurations[ii]));
        probs.push_back(exp(lProbs.back()));
        masses.push_back(calc_mass(configurations[ii], atom_masses, isotopeNo));
    }

    lProbs.push_back(-std::numeric_limits<double>::infinity());  // Restore guardian

    sorted_up_to_idx = configurations.size();
    guarded_lProbs = lProbs.data()+1;  // Vector might have reallocated its backing storage

    return true;
}


double LayeredMarginal::get_min_mass() const
{
    double ret = std::numeric_limits<double>::infinity();
    for(std::vector<double>::const_iterator it = masses.begin(); it != masses.end(); ++it)
        if(*it < ret)
            ret = *it;
    return ret;
}


double LayeredMarginal::get_max_mass() const
{
    double ret = -std::numeric_limits<double>::infinity();
    for(std::vector<double>::const_iterator it = masses.begin(); it != masses.end(); ++it)
        if(*it > ret)
            ret = *it;
    return ret;
}

}  // namespace IsoSpec
