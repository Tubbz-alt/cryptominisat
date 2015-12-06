/*
Copyright (c) 2010-2015 Mate Soos
Copyright (c) Kuldeep S. Meel, Daniel J. Fremont

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#if defined(__GNUC__) && defined(__linux__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fenv.h>
#endif

#include <ctime>
#include <cstring>
#include <errno.h>
#include <string.h>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <set>
#include <fstream>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <list>
#include <array>

#ifdef USE_PTHREADS
#include <thread>
#endif

#include "main.h"
#include "main_common.h"
#include "time_mem.h"
#include "dimacsparser.h"
#include "cryptominisat4/cryptominisat.h"

#ifdef USE_ZLIB
static size_t gz_read(void* buf, size_t num, size_t count, gzFile f)
{
    return gzread(f, buf, num*count);
}
#endif


#include <boost/lexical_cast.hpp>
using namespace CMSat;
using boost::lexical_cast;

using std::cout;
using std::cerr;
using std::endl;
using boost::lexical_cast;
using std::list;
using std::map;

struct WrongParam
{
    WrongParam(string _param, string _msg) :
        param(_param)
        , msg(_msg)
    {}

    const string& getMsg() const
    {
        return msg;
    }

    const string& getParam() const
    {
        return param;
    }

    string param;
    string msg;
};

bool fileExists(const string& filename)
{
    struct stat buf;
    if (stat(filename.c_str(), &buf) != -1)
    {
        return true;
    }
    return false;
}


Main::Main(int _argc, char** _argv) :
    fileNamePresent (false)
    , argc(_argc)
    , argv(_argv)
{
}

SATSolver* solverToInterrupt;
int clear_interrupt;
string redDumpFname;
string irredDumpFname;
bool unisolve = false;

void SIGINT_handler(int)
{
    SATSolver* solver = solverToInterrupt;
    cout << "c " << endl;
    std::cerr << "*** INTERRUPTED ***" << endl;
    if (!redDumpFname.empty() || !irredDumpFname.empty() || clear_interrupt) {
        solver->interrupt_asap();
        std::cerr
        << "*** Please wait. We need to interrupt cleanly" << endl
        << "*** This means we might need to finish some calculations"
        << endl;
    } else {
        if (solver->nVars() > 0) {
            //if (conf.verbosity >= 1) {
                solver->add_in_partial_solving_stats();
                solver->print_stats();
            //}
        } else {
            cout
            << "No clauses or variables were put into the solver, exiting without stats"
            << endl;
        }
        _exit(1);
    }
}

void Main::readInAFile(SATSolver* solver2, const string& filename)
{
    solver2->add_sql_tag("filename", filename);
    if (conf.verbosity >= 1) {
        cout << "c Reading file '" << filename << "'" << endl;
    }
    #ifndef USE_ZLIB
    FILE * in = fopen(filename.c_str(), "rb");
    DimacsParser<StreamBuffer<FILE*, fread_op_norm, fread> > parser(solver2, debugLib, conf.verbosity);
    #else
    gzFile in = gzopen(filename.c_str(), "rb");
    DimacsParser<StreamBuffer<gzFile, fread_op_zip, gz_read> > parser(solver2, debugLib, conf.verbosity);
    #endif

    if (in == NULL) {
        std::cerr
        << "ERROR! Could not open file '"
        << filename
        << "' for reading: " << strerror(errno) << endl;

        std::exit(1);
    }

    if (!parser.parse_DIMACS(in)) {
        exit(-1);
    }
    independent_vars = parser.independent_vars;
    if (independent_vars.empty()) {
        for(size_t i = 0; i < solver->nVars(); i++) {
            independent_vars.push_back(i);
        }
    }

    #ifndef USE_ZLIB
        fclose(in);
    #else
        gzclose(in);
    #endif
}

void Main::readInStandardInput(SATSolver* solver2)
{
    if (conf.verbosity) {
        cout
        << "c Reading from standard input... Use '-h' or '--help' for help."
        << endl;
    }

    #ifndef USE_ZLIB
    FILE * in = stdin;
    #else
    gzFile in = gzdopen(fileno(stdin), "rb");
    #endif

    if (in == NULL) {
        std::cerr << "ERROR! Could not open standard input for reading" << endl;
        std::exit(1);
    }

    #ifndef USE_ZLIB
    DimacsParser<StreamBuffer<FILE*, fread_op_norm, fread> > parser(solver2, debugLib, conf.verbosity);
    #else
    DimacsParser<StreamBuffer<gzFile, fread_op_zip, gz_read> > parser(solver2, debugLib, conf.verbosity);
    #endif

    if (!parser.parse_DIMACS(in)) {
        exit(-1);
    }

    #ifdef USE_ZLIB
        gzclose(in);
    #endif
}

void Main::parseInAllFiles(SATSolver* solver2)
{
    const double myTime = cpuTime();

    //First read normal extra files
    if (!debugLib.empty() && filesToRead.size() > 1) {
        cout
        << "debugLib must be OFF"
        << "to parse in more than one file"
        << endl;

        std::exit(-1);
    }

    for (const string& fname: filesToRead) {
        readInAFile(solver2, fname.c_str());
    }

    if (!fileNamePresent) {
        readInStandardInput(solver2);
    }

    if (conf.verbosity >= 1) {
        cout
        << "c Parsing time: "
        << std::fixed << std::setprecision(2)
        << (cpuTime() - myTime)
        << " s" << endl;
    }
}

void Main::printResultFunc(
    std::ostream* os
    , const bool toFile
    , const lbool ret
) {
    if (ret == l_True) {
        if(toFile) {
            *os << "SAT" << endl;
        }
        else if (!printResult) *os << "s SATISFIABLE" << endl;
        else                   *os << "s SATISFIABLE" << endl;
     } else if (ret == l_False) {
        if(toFile) {
            *os << "UNSAT" << endl;
        }
        else if (!printResult) *os << "s UNSATISFIABLE" << endl;
        else                   *os << "s UNSATISFIABLE" << endl;
    } else {
        *os << "s INDETERMINATE" << endl;
    }

    if (ret == l_True && (printResult || toFile)) {
        if (toFile) {
            for (uint32_t var = 0; var < solver->nVars(); var++) {
                if (solver->get_model()[var] != l_Undef) {
                    *os << ((solver->get_model()[var] == l_True)? "" : "-") << var+1 << " ";
                }
            }
            *os << "0" << endl;
        } else {
            print_model(os, solver);
        }
    }
}

void Main::add_supported_options()
{
    // Declare the supported options.
    po::options_description generalOptions("Most important options");
    generalOptions.add_options()
    ("help,h", "Print simple help")
    ("hhelp", "Print extensive help")
    ("version,v", "Print version info")
    ("verb", po::value(&conf.verbosity)->default_value(conf.verbosity)
        , "[0-10] Verbosity of solver. 0 = only solution")
    ("random,r", po::value(&conf.origSeed)->default_value(conf.origSeed)
        , "[0..] Random seed")
    ("threads,t", po::value(&num_threads)->default_value(1)
        ,"Number of threads")
    ("sync", po::value(&conf.sync_every_confl)->default_value(conf.sync_every_confl)
        , "Sync threads every N conflicts")
    ("maxtime", po::value(&conf.maxTime)->default_value(conf.maxTime, "MAX")
        , "Stop solving after this much time (s)")
    ("maxconfl", po::value(&conf.maxConfl)->default_value(conf.maxConfl, "MAX")
        , "Stop solving after this many conflicts")
    ("mult,m", po::value(&conf.orig_global_timeout_multiplier)->default_value(conf.orig_global_timeout_multiplier)
        , "Multiplier for all simplification cutoffs")
    ("preproc,p", po::value(&conf.preprocess)->default_value(conf.preprocess)
        , "0 = normal run, 1 = preprocess and dump, 2 = read back dump and solution to produce final solution")
    //("greedyunbound", po::bool_switch(&conf.greedyUnbound)
    //    , "Greedily unbound variables that are not needed for SAT")
    ;

    std::ostringstream s_blocking_multip;
    s_blocking_multip << std::setprecision(4) << conf.blocking_restart_multip;

    po::options_description restartOptions("Restart options");
    restartOptions.add_options()
    ("restart", po::value<string>()
        , "{geom, glue, luby}  Restart strategy to follow.")
    ("gluehist", po::value(&conf.shortTermHistorySize)->default_value(conf.shortTermHistorySize)
        , "The size of the moving window for short-term glue history of redundant clauses. If higher, the minimal number of conflicts between restarts is longer")
    ("blkrest", po::value(&conf.do_blocking_restart)->default_value(conf.do_blocking_restart)
        , "Perform blocking restarts as per Glucose 3.0")
    ("blkrestlen", po::value(&conf.blocking_restart_trail_hist_length)->default_value(conf.blocking_restart_trail_hist_length)
        , "Length of the long term trail size for blocking restart")
    ("blkrestmultip", po::value(&conf.blocking_restart_multip)->default_value(conf.blocking_restart_multip, s_blocking_multip.str())
        , "Multiplier used for blocking restart cut-off (called 'R' in Glucose 3.0)")
    ("lwrbndblkrest", po::value(&conf.lower_bound_for_blocking_restart)->default_value(conf.lower_bound_for_blocking_restart)
        , "Lower bound on blocking restart -- don't block before this many concflicts")
    ;

    std::ostringstream s_incclean;

    std::ostringstream s_clean_confl_multiplier;
    s_clean_confl_multiplier << std::setprecision(2) << conf.clean_confl_multiplier;

    po::options_description reduceDBOptions("Red clause removal options");
    reduceDBOptions.add_options()
    ("cleanconflmult", po::value(&conf.clean_confl_multiplier)->default_value(conf.clean_confl_multiplier, s_clean_confl_multiplier.str())
        , "If prop&confl are used to clean, by what value should we multiply the conflicts relative to propagations (conflicts are much more rare, but maybe more useful)")
    ("clearstat", po::value(&conf.doClearStatEveryClauseCleaning)->default_value(conf.doClearStatEveryClauseCleaning)
        , "Clear clause statistics data of each clause after clause cleaning")
    ("incclean", po::value(&conf.inc_max_temp_red_cls)->default_value(conf.inc_max_temp_red_cls, s_incclean.str())
        , "Clean increment cleaning by this factor for next cleaning")
    ("maxredratio", po::value(&conf.maxNumRedsRatio)->default_value(conf.maxNumRedsRatio)
        , "Don't ever have more than maxNumRedsRatio*(irred_clauses) redundant clauses")
    ("maxtemp", po::value(&conf.max_temporary_learnt_clauses)->default_value(conf.max_temporary_learnt_clauses)
        , "Maximum number of temporary clauses of high glue")
    ;

    std::ostringstream s_random_var_freq;
    s_random_var_freq << std::setprecision(5) << conf.random_var_freq;

    std::ostringstream s_var_decay_start;
    s_var_decay_start << std::setprecision(5) << conf.var_decay_start;

    std::ostringstream s_var_decay_max;
    s_var_decay_max << std::setprecision(5) << conf.var_decay_max;

    po::options_description varPickOptions("Variable branching options");
    varPickOptions.add_options()
    ("vardecaystart", po::value(&conf.var_decay_start)->default_value(conf.var_decay_start, s_var_decay_start.str())
        , "variable activity increase divider (MUST be smaller than multiplier)")
    ("vardecaymax", po::value(&conf.var_decay_max)->default_value(conf.var_decay_max, s_var_decay_max.str())
        , "variable activity increase divider (MUST be smaller than multiplier)")
    ("vincstart", po::value(&conf.var_inc_start)->default_value(conf.var_inc_start)
        , "variable activity increase stars with this value. Make sure that this multiplied by multiplier and dividied by divider is larger than itself")
    ("freq", po::value(&conf.random_var_freq)->default_value(conf.random_var_freq, s_random_var_freq.str())
        , "[0 - 1] freq. of picking var at random")
    ("dompickf", po::value(&conf.dominPickFreq)->default_value(conf.dominPickFreq)
        , "Use dominating literal every once in N when picking decision literal")
    ("morebump", po::value(&conf.extra_bump_var_activities_based_on_glue)->default_value(conf.extra_bump_var_activities_based_on_glue)
        , "Bump variables' activities based on the glue of red clauses there are in during UIP generation (as per Glucose)")
    ;

    po::options_description polar_options("Variable polarity options");
    polar_options.add_options()
    ("polar", po::value<string>()->default_value("auto")
        , "{true,false,rnd,auto} Selects polarity mode. 'true' -> selects only positive polarity when branching. 'false' -> selects only negative polarity when brancing. 'auto' -> selects last polarity used (also called 'caching')")
    ("calcpolar1st", po::value(&conf.do_calc_polarity_first_time)->default_value(conf.do_calc_polarity_first_time)
        , "Calculate the polarity of variables based on their occurrences at startup of solve()")
    ("calcpolarall", po::value(&conf.do_calc_polarity_every_time)->default_value(conf.do_calc_polarity_every_time)
        , "Calculate the polarity of variables based on their occurrences at startup & after every simplification")
    ;


    po::options_description iterativeOptions("Iterative solve options");
    iterativeOptions.add_options()
    ("maxsol", po::value(&max_nr_of_solutions)->default_value(max_nr_of_solutions)
        , "Search for given amount of solutions")
    ("dumpred", po::value(&redDumpFname)
        , "If stopped dump redundant clauses here")
    ("maxdump", po::value(&conf.maxDumpRedsSize)
        , "Maximum length of redundant clause dumped")
    ("dumpirred", po::value(&irredDumpFname)
        , "If stopped, dump irred original problem here")
    ("debuglib", po::value<string>(&debugLib)
        , "MainSolver at specific 'solve()' points in CNF file")
    ("dumpresult", po::value(&resultFilename)
        , "Write result(s) to this file")
    ;

    po::options_description probeOptions("Probing options");
    probeOptions.add_options()
    ("bothprop", po::value(&conf.doBothProp)->default_value(conf.doBothProp)
        , "Do propagations solely to propagate the same value twice")
    ("probe", po::value(&conf.doProbe)->default_value(conf.doProbe)
        , "Carry out probing")
    ("probemaxm", po::value(&conf.probe_bogoprops_time_limitM)->default_value(conf.probe_bogoprops_time_limitM)
      , "Time in mega-bogoprops to perform probing")
    ("transred", po::value(&conf.doTransRed)->default_value(conf.doTransRed)
        , "Remove useless binary clauses (transitive reduction)")
    ("intree", po::value(&conf.doIntreeProbe)->default_value(conf.doIntreeProbe)
        , "Carry out intree-based probing")
    ("intreemaxm", po::value(&conf.intree_time_limitM)->default_value(conf.intree_time_limitM)
      , "Time in mega-bogoprops to perform intree probing")
    ;

    std::ostringstream ssERatio;
    ssERatio << std::setprecision(4) << "norm: " << conf.varElimRatioPerIter << " preproc: " << 1.0;

    po::options_description simplificationOptions("Simplification options");
    simplificationOptions.add_options()
    ("schedsimp", po::value(&conf.do_simplify_problem)->default_value(conf.do_simplify_problem)
        , "Perform simplification rounds. If 0, we never perform any.")
    ("presimp", po::value(&conf.simplify_at_startup)->default_value(conf.simplify_at_startup)
        , "Perform simplification at the very start")
    ("nonstop,n", po::value(&conf.never_stop_search)->default_value(conf.never_stop_search)
        , "Never stop the search() process in class SATSolver")

    ("schedule", po::value(&conf.simplify_schedule_nonstartup)
        , "Schedule for simplification during run")
    ("preschedule", po::value(&conf.simplify_schedule_startup)
        , "Schedule for simplification at startup")

    ("occsimp", po::value(&conf.perform_occur_based_simp)->default_value(conf.perform_occur_based_simp)
        , "Perform occurrence-list-based optimisations (variable elimination, subsumption, bounded variable addition...)")


    ("confbtwsimp", po::value(&conf.num_conflicts_of_search)->default_value(conf.num_conflicts_of_search)
        , "Start first simplification after this many conflicts")
    ("confbtwsimpinc", po::value(&conf.num_conflicts_of_search_inc)->default_value(conf.num_conflicts_of_search_inc)
        , "Simp rounds increment by this power of N")
    ("varelim", po::value(&conf.doVarElim)->default_value(conf.doVarElim)
        , "Perform variable elimination as per Een and Biere")
    ("varelimto", po::value(&conf.varelim_time_limitM)->default_value(conf.varelim_time_limitM)
        , "Var elimination bogoprops M time limit")
    ("emptyelim", po::value(&conf.do_empty_varelim)->default_value(conf.do_empty_varelim)
        , "Perform empty resolvent elimination using bit-map trick")
    ("elimstrgy", po::value(&var_elim_strategy)->default_value(getNameOfElimStrategy(conf.var_elim_strategy))
        , "Sort variable elimination order by intelligent guessing ('heuristic') or by exact calculation ('calculate')")
    ("elimcplxupd", po::value(&conf.updateVarElimComplexityOTF)->default_value(conf.updateVarElimComplexityOTF)
        , "Update estimated elimination complexity on-the-fly while eliminating")
    ("elimcoststrategy", po::value(&conf.varElimCostEstimateStrategy)->default_value(conf.varElimCostEstimateStrategy)
        , "How simple strategy (guessing, above) is calculated. Valid values: 0, 1")
    ("strengthen", po::value(&conf.do_strengthen_with_occur)->default_value(conf.do_strengthen_with_occur)
        , "Perform clause contraction through self-subsuming resolution as part of the occurrence-subsumption system")
    ("bva", po::value(&conf.do_bva)->default_value(conf.do_bva)
        , "Perform bounded variable addition")
    ("bvalim", po::value(&conf.bva_limit_per_call)->default_value(conf.bva_limit_per_call)
        , "Maximum number of variables to add by BVA per call")
    ("bva2lit", po::value(&conf.bva_also_twolit_diff)->default_value(conf.bva_also_twolit_diff)
        , "BVA with 2-lit difference hack, too. Beware, this reduces the effectiveness of 1-lit diff")
    ("bvato", po::value(&conf.bva_time_limitM)->default_value(conf.bva_time_limitM)
        , "BVA time limit in bogoprops M")
    ("noextbinsubs", po::value(&conf.doExtBinSubs)->default_value(conf.doExtBinSubs)
        , "No extended subsumption with binary clauses")
    ("eratio", po::value(&conf.varElimRatioPerIter)->default_value(conf.varElimRatioPerIter, ssERatio.str())
        , "Eliminate this ratio of free variables at most per variable elimination iteration")
    ("skipresol", po::value(&conf.skip_some_bve_resolvents)->default_value(conf.skip_some_bve_resolvents)
        , "Skip BVE resolvents in case they belong to a gate")
    ("occredmax", po::value(&conf.maxRedLinkInSize)->default_value(conf.maxRedLinkInSize)
        , "Don't add to occur list any redundant clause larger than this")
    ("occirredmaxmb", po::value(&conf.maxOccurIrredMB)->default_value(conf.maxOccurIrredMB)
        , "Don't allow irredundant occur size to be beyond this many MB")
    ("occredmaxmb", po::value(&conf.maxOccurRedMB)->default_value(conf.maxOccurRedMB)
        , "Don't allow redundant occur size to be beyond this many MB")
    ("substimelim", po::value(&conf.subsumption_time_limitM)->default_value(conf.subsumption_time_limitM)
        , "Time-out in bogoprops M of subsumption of long clauses with long clauses, after computing occur")
    ("substimelim", po::value(&conf.strengthening_time_limitM)->default_value(conf.strengthening_time_limitM)
        , "Time-out in bogoprops M of strengthening of long clauses with long clauses, after computing occur")
    ("substimelim", po::value(&conf.aggressive_elim_time_limitM)->default_value(conf.aggressive_elim_time_limitM)
        , "Time-out in bogoprops M of agressive(=uses reverse distillation) var-elimination")
    ;

    std::ostringstream sccFindPercent;
    sccFindPercent << std::fixed << std::setprecision(3) << conf.sccFindPercent;

    po::options_description xorOptions("XOR-related options");
    xorOptions.add_options()
    ("xor", po::value(&conf.doFindXors)->default_value(conf.doFindXors)
        , "Discover long XORs")
    ("xorcache", po::value(&conf.useCacheWhenFindingXors)->default_value(conf.useCacheWhenFindingXors)
        , "Use cache when finding XORs. Finds a LOT more XORs, but takes a lot more time")
    ("echelonxor", po::value(&conf.doEchelonizeXOR)->default_value(conf.doEchelonizeXOR)
        , "Extract data from XORs through echelonization (TOP LEVEL ONLY)")
    ("maxxormat", po::value(&conf.maxXORMatrix)->default_value(conf.maxXORMatrix)
        , "Maximum matrix size (=num elements) that we should try to echelonize")
    //Not implemented yet
    //("mix", po::value(&conf.doMixXorAndGates)->default_value(conf.doMixXorAndGates)
    //    , "Mix XORs and OrGates for new truths")
    ;

    po::options_description eqLitOpts("Equivalent literal options");
    eqLitOpts.add_options()
    ("scc", po::value(&conf.doFindAndReplaceEqLits)->default_value(conf.doFindAndReplaceEqLits)
        , "Find equivalent literals through SCC and replace them")
    ("extscc", po::value(&conf.doExtendedSCC)->default_value(conf.doExtendedSCC)
        , "Perform SCC using cache")
    ("sccperc", po::value(&conf.sccFindPercent)->default_value(conf.sccFindPercent, sccFindPercent.str())
        , "Perform SCC only if the number of new binary clauses is at least this many % of the number of free variables")
    ;

    po::options_description gateOptions("Gate-related options");
    gateOptions.add_options()
    ("gates", po::value(&conf.doGateFind)->default_value(conf.doGateFind)
        , "Find gates. Disables all sub-options below")
    ("gorshort", po::value(&conf.doShortenWithOrGates)->default_value(conf.doShortenWithOrGates)
        , "Shorten clauses with OR gates")
    ("gandrem", po::value(&conf.doRemClWithAndGates)->default_value(conf.doRemClWithAndGates)
        , "Remove clauses with AND gates")
    ("gateeqlit", po::value(&conf.doFindEqLitsWithGates)->default_value(conf.doFindEqLitsWithGates)
        , "Find equivalent literals using gates")
    /*("maxgatesz", po::value(&conf.maxGateSize)->default_value(conf.maxGateSize)
        , "Maximum gate size to discover")*/
    ("printgatedot", po::value(&conf.doPrintGateDot)->default_value(conf.doPrintGateDot)
        , "Print gate structure regularly to file 'gatesX.dot'")
    ("gatefindto", po::value(&conf.gatefinder_time_limitM)->default_value(conf.gatefinder_time_limitM)
        , "Max time in bogoprops M to find gates")
    ("shortwithgatesto", po::value(&conf.shorten_with_gates_time_limitM)->default_value(conf.shorten_with_gates_time_limitM)
        , "Max time to shorten with gates, bogoprops M")
    ("remwithgatesto", po::value(&conf.remove_cl_with_gates_time_limitM)->default_value(conf.remove_cl_with_gates_time_limitM)
        , "Max time to remove with gates, bogoprops M")
    ;

    po::options_description conflOptions("Conflict options");
    conflOptions.add_options()
    ("recur", po::value(&conf.doRecursiveMinim)->default_value(conf.doRecursiveMinim)
        , "Perform recursive minimisation")
    ("moreminim", po::value(&conf.doMinimRedMore)->default_value(conf.doMinimRedMore)
        , "Perform strong minimisation at conflict gen.")
    ("moreminimcache", po::value(&conf.more_red_minim_limit_cache)->default_value(conf.more_red_minim_limit_cache)
        , "Time-out in microsteps for each more minimisation with cache. Only active if 'moreminim' is on")
    ("moreminimbin", po::value(&conf.more_red_minim_limit_binary)->default_value(conf.more_red_minim_limit_binary)
        , "Time-out in microsteps for each more minimisation with binary clauses. Only active if 'moreminim' is on")
    ("moreminimlit", po::value(&conf.max_num_lits_more_red_min)->default_value(conf.max_num_lits_more_red_min)
        , "Number of first literals to look through for more minimisation when doing learnt cl minim right after learning it")
    ("cacheformoreminim", po::value(&conf.more_otf_shrink_with_stamp)->default_value(conf.more_otf_shrink_with_stamp)
        , "Use cache for otf more minim of learnt clauses")
    ("stampformoreminim", po::value(&conf.more_otf_shrink_with_cache)->default_value(conf.more_otf_shrink_with_cache)
        , "Use stamp for otf more minim of learnt clauses")
    ("alwaysmoremin", po::value(&conf.doAlwaysFMinim)->default_value(conf.doAlwaysFMinim)
        , "Always strong-minimise clause")
    ("otfsubsume", po::value(&conf.doOTFSubsume)->default_value(conf.doOTFSubsume)
        , "Perform on-the-fly subsumption")
    ("rewardotfsubsume", po::value(&conf.rewardShortenedClauseWithConfl)
        ->default_value(conf.rewardShortenedClauseWithConfl)
        , "Reward with this many prop&confl a clause that has been shortened with on-the-fly subsumption")
    ("printimpldot", po::value(&conf.doPrintConflDot)->default_value(conf.doPrintConflDot)
        , "Print implication graph DOT files (for input into graphviz package)")
    ;

    po::options_description propOptions("Propagation options");
    propOptions.add_options()
    ("updateglueonprop", po::value(&conf.update_glues_on_prop)->default_value(conf.update_glues_on_prop)
        , "Update glues while propagating")
    ("updateglueonanalysis", po::value(&conf.update_glues_on_analyze)->default_value(conf.update_glues_on_analyze)
        , "Update glues while analyzing")
    ("binpri", po::value(&conf.propBinFirst)->default_value(conf.propBinFirst)
        , "Propagated binary clauses strictly first")
    ("otfhyper", po::value(&conf.otfHyperbin)->default_value(conf.otfHyperbin)
        , "Perform hyper-binary resolution at dec. level 1 after every restart and during probing")
    ;


    po::options_description stampOptions("Stamping options");
    stampOptions.add_options()
    ("stamp", po::value(&conf.doStamp)->default_value(conf.doStamp)
        , "Use time stamping as per Heule&Jarvisalo&Biere paper")
    ("cache", po::value(&conf.doCache)->default_value(conf.doCache)
        , "Use implication cache -- may use a lot of memory")
    ("cachesize", po::value(&conf.maxCacheSizeMB)->default_value(conf.maxCacheSizeMB)
        , "Maximum size of the implication cache in MB. It may temporarily reach higher usage, but will be deleted&disabled if this limit is reached.")
    ("calcreach", po::value(&conf.doCalcReach)->default_value(conf.doCalcReach)
        , "Calculate literal reachability")
    ("cachecutoff", po::value(&conf.cacheUpdateCutoff)->default_value(conf.cacheUpdateCutoff)
        , "If the number of literals propagated by a literal is more than this, it's not included into the implication cache")
    ;

    po::options_description sqlOptions("SQL options");
    sqlOptions.add_options()
    ("sql", po::value(&conf.doSQL)->default_value(conf.doSQL)
        , "Write to SQL. 0 = don't attempt to writ to DB, 1 = try but continue if fails, 2 = abort if cannot write to DB")
    ("wsql", po::value(&conf.whichSQL)->default_value(0)
        , "0 = prefer MySQL \
1 = prefer SQLite, \
2 = only use MySQL, \
3 = only use SQLite" )
    ("sqlitedb", po::value(&conf.sqlite_filename)
        , "Where to put the SQLite database")
    ("sqluser", po::value(&conf.sqlUser)->default_value(conf.sqlUser)
        , "SQL user to connect with")
    ("sqlpass", po::value(&conf.sqlPass)->default_value(conf.sqlPass)
        , "SQL user's pass to connect with")
    ("sqldb", po::value(&conf.sqlDatabase)->default_value(conf.sqlDatabase)
        , "SQL database name. Default is used by PHP system, so it's highly recommended")
    ("sqlserver", po::value(&conf.sqlServer)->default_value(conf.sqlServer)
        , "SQL server hostname/IP")
    ("sqlrestfull", po::value(&conf.dump_individual_restarts)->default_value(conf.dump_individual_restarts)
        , "Dump individual restart statistics in FULL")
    ("sqlresttime", po::value(&conf.dump_individual_search_time)->default_value(conf.dump_individual_search_time)
        , "Dump individual time for restart stats, but ONLY time")
    ;

    po::options_description printOptions("Printing options");
    printOptions.add_options()
    ("verbstat", po::value(&conf.verbStats)->default_value(conf.verbStats)
        , "Change verbosity of statistics at the end of the solving [0..2]")
    ("printfull", po::value(&conf.print_all_stats)->default_value(conf.print_all_stats)
        , "Print more thorough, but different stats")
    ("printsol,s", po::value(&printResult)->default_value(printResult)
        , "Print assignment if solution is SAT")
    ("restartprint", po::value(&conf.print_restart_line_every_n_confl)->default_value(conf.print_restart_line_every_n_confl)
        , "Print restart status lines at least every N conflicts")
    ;

    po::options_description componentOptions("Component options");
    componentOptions.add_options()
    ("comps", po::value(&conf.doCompHandler)->default_value(conf.doCompHandler)
        , "Perform component-finding and separate handling")
    ("compsfrom", po::value(&conf.handlerFromSimpNum)->default_value(conf.handlerFromSimpNum)
        , "Component finding only after this many simplification rounds")
    ("compsvar", po::value(&conf.compVarLimit)->default_value(conf.compVarLimit)
        , "Only use components in case the number of variables is below this limit")
    ("compslimit", po::value(&conf.comp_find_time_limitM)->default_value(conf.comp_find_time_limitM)
        , "Limit how much time is spent in component-finding");

    po::options_description miscOptions("Simplification options");
    miscOptions.add_options()
    //("noparts", "Don't find&solve subproblems with subsolvers")
    ("distill", po::value(&conf.do_distill_clauses)->default_value(conf.do_distill_clauses)
        , "Regularly execute clause distillation")
    ("distillmaxm", po::value(&conf.distill_long_irred_cls_time_limitM)->default_value(conf.distill_long_irred_cls_time_limitM)
        , "Maximum number of Mega-bogoprops(~time) to spend on viviying long irred cls by enqueueing and propagating")
    ("distillto", po::value(&conf.distill_time_limitM)->default_value(conf.distill_time_limitM)
        , "Maximum time in bogoprops M for distillation")
    ("distillby", po::value(&conf.distill_queue_by)->default_value(conf.distill_queue_by)
        , "Enqueue lits from long clauses during distiallation N-by-N. 1 is slower, 2 is faster, etc.")
    ("strcachemaxm", po::value(&conf.watch_cache_stamp_based_str_time_limitM)->default_value(conf.watch_cache_stamp_based_str_time_limitM)
        , "Maximum number of Mega-bogoprops(~time) to spend on viviying long irred cls through watches, cache and stamps")
    ("sortwatched", po::value(&conf.doSortWatched)->default_value(conf.doSortWatched)
        , "Sort watches according to size")
    ("renumber", po::value(&conf.doRenumberVars)->default_value(conf.doRenumberVars)
        , "Renumber variables to increase CPU cache efficiency")
    ("savemem", po::value(&conf.doSaveMem)->default_value(conf.doSaveMem)
        , "Save memory by deallocating variable space after renumbering. Only works if renumbering is active.")
    ("implicitmanip", po::value(&conf.doStrSubImplicit)->default_value(conf.doStrSubImplicit)
        , "Subsume and strengthen implicit clauses with each other")
    ("implsubsto", po::value(&conf.subsume_implicit_time_limitM)->default_value(conf.subsume_implicit_time_limitM)
        , "Timeout (in bogoprop Millions) of implicit subsumption")
    ("implstrto", po::value(&conf.distill_implicit_with_implicit_time_limitM)->default_value(conf.distill_implicit_with_implicit_time_limitM)
        , "Timeout (in bogoprop Millions) of implicit strengthening")
    ("burst", po::value(&conf.burst_search_len)->default_value(conf.burst_search_len)
        , "Number of conflicts to do in burst search")
    ;

    po::options_description hiddenOptions("Debug options for fuzzing, weird options not exposed");
    hiddenOptions.add_options()
    ("drupdebug", po::bool_switch(&drupDebug)
        , "Output DRUP verification into the console. Helpful to see where DRUP fails -- use in conjunction with --verb 20")
    ("clearinter", po::value(&clear_interrupt)->default_value(0)
        , "Interrupt threads cleanly, all the time")
    ("zero-exit-status", po::bool_switch(&zero_exit_status)
        , "Exit with status zero in case the solving has finished without an issue")
    ("input", po::value< vector<string> >(), "file(s) to read")
    ("reconfat", po::value(&conf.reconfigure_at)->default_value(conf.reconfigure_at)
        , "Reconfigure after this many simplifications")
    ("printtimes", po::value(&conf.do_print_times)->default_value(conf.do_print_times)
        , "Print time it took for each simplification run. If set to 0, logs are easier to compare")
    ("drup,d", po::value(&drupfilname)
        , "Put DRUP verification information into this file")
    ("reconf", po::value(&conf.reconfigure_val)->default_value(conf.reconfigure_val)
        , "Reconfigure after some time to this solver configuration [0..13]")
    ("savedstate", po::value(&conf.saved_state_file)->default_value(conf.saved_state_file)
        , "The file to save the saved state of the solver")
    ;

#ifdef USE_GAUSS
    po::options_description gaussOptions("Gauss options");
    gaussOptions.add_options()
    ("iterreduce", po::value(&conf.gaussconf.iterativeReduce)->default_value(conf.gaussconf.iterativeReduce)
        , "Don't reduce iteratively the matrix that is updated")
    ("maxmatrixrows", po::value(&conf.gaussconf.max_matrix_rows)->default_value(conf.gaussconf.max_matrix_rows)
        , "Set maximum no. of rows for gaussian matrix. Too large matrixes"
        "should bee discarded for reasons of efficiency")
    ("autodisablegauss", po::value(&conf.gaussconf.autodisable)->default_value(conf.gaussconf.autodisable)
        , "Automatically disable gauss when performing badly")
    ("minmatrixrows", po::value(&conf.gaussconf.min_matrix_rows)->default_value(conf.gaussconf.min_matrix_rows)
        , "Set minimum no. of rows for gaussian matrix. Normally, too small"
        "matrixes are discarded for reasons of efficiency")
    ("savematrix", po::value(&conf.gaussconf.only_nth_gauss_save)->default_value(conf.gaussconf.only_nth_gauss_save)
        , "Save matrix every Nth decision level.")
    ("maxnummatrixes", po::value(&conf.gaussconf.max_num_matrixes)->default_value(conf.gaussconf.max_num_matrixes)
        , "Maximum number of matrixes to treat.")
    ;
#endif //USE_GAUSS

    po::options_description approxMCOptions("ApproxMC options");
    approxMCOptions.add_options()
    ("samples", po::value(&conf.samples)->default_value(conf.samples), "")
    ("callsPerSolver", po::value(&conf.callsPerSolver)->default_value(conf.callsPerSolver), "")
    ("pivotAC", po::value(&conf.pivotApproxMC)->default_value(conf.pivotApproxMC), "")
    ("pivotUniGen", po::value(&conf.pivotUniGen)->default_value(conf.pivotUniGen), "")
    ("kappa", po::value(&conf.kappa)->default_value(conf.kappa), "")
    ("tApproxMC", po::value(&conf.tApproxMC)->default_value(conf.tApproxMC), "")
    ("startIteration", po::value(&conf.startIteration)->default_value(conf.startIteration), "")
    ("multisample", po::value(&conf.multisample)->default_value(conf.multisample), "")
    ("aggregation", po::value(&conf.aggregateSolutions)->default_value(conf.aggregateSolutions), "")
    ("uni", po::bool_switch(&unisolve), "Use unisolve system")
    ;

    p.add("input", 1);
    p.add("drup", 1);

    help_options_complicated
    .add(generalOptions)
    #if defined(USE_MYSQL) or defined(USE_SQLITE3)
    .add(sqlOptions)
    #endif
    .add(restartOptions)
    .add(printOptions)
    .add(propOptions)
    .add(reduceDBOptions)
    .add(varPickOptions)
    .add(polar_options)
    .add(conflOptions)
    .add(iterativeOptions)
    .add(probeOptions)
    .add(stampOptions)
    .add(simplificationOptions)
    .add(eqLitOpts)
    .add(componentOptions)
    #ifdef USE_M4RI
    .add(xorOptions)
    #endif
    .add(gateOptions)
    #ifdef USE_GAUSS
    .add(gaussOptions)
    #endif
    .add(approxMCOptions)
    .add(miscOptions)
    ;

    all_options.add(help_options_complicated);
    all_options.add(hiddenOptions);

    help_options_simple
    .add(generalOptions)
    ;
}

void Main::check_options_correctness()
{
    try {
        po::store(po::command_line_parser(argc, argv).options(all_options).positional(p).run(), vm);
        if (vm.count("hhelp"))
        {
            cout
            << "USAGE 1: " << argv[0] << " [options] inputfile [drat-trim-file]" << endl
            << "USAGE 2: " << argv[0] << " --preproc 1 [options] inputfile simplified-cnf-file" << endl
            << "USAGE 2: " << argv[0] << " --preproc 2 [options] solution-file" << endl

            << " where input is "
            #ifndef USE_ZLIB
            << "plain"
            #else
            << "plain or gzipped"
            #endif
            << " DIMACS." << endl;

            cout << help_options_complicated << endl;
            cout << "NORMAL RUN SCHEDULES" << endl;
            cout << "--------------------" << endl;
            cout << "Default schedule: " << conf.simplify_schedule_nonstartup << endl;
            cout << "Default schedule at startup: " << conf.simplify_schedule_startup << endl << endl;

            cout << "PREPROC RUN SCHEDULES" << endl;
            cout << "--------------------" << endl;
            cout << "Default schedule: " << conf.simplify_schedule_preproc<< endl;
            std::exit(0);
        }

        if (vm.count("help"))
        {
            cout
            << "USAGE 1: " << argv[0] << " [options] inputfile [drat-trim-file]" << endl
            << "USAGE 2: " << argv[0] << " --preproc 1 [options] inputfile simplified-cnf-file" << endl
            << "USAGE 2: " << argv[0] << " --preproc 2 [options] solution-file" << endl

            << " where input is "
            #ifndef USE_ZLIB
            << "plain"
            #else
            << "plain or gzipped"
            #endif
            << " DIMACS." << endl;

            cout << help_options_simple << endl;
            std::exit(0);
        }

        po::notify(vm);
    } catch (boost::exception_detail::clone_impl<
        boost::exception_detail::error_info_injector<po::unknown_option> >& c
    ) {
        cerr
        << "ERROR: Some option you gave was wrong. Please give '--help' to get help" << endl
        << "       Unkown option: " << c.what() << endl;
        std::exit(-1);
    } catch (boost::bad_any_cast &e) {
        std::cerr
        << "ERROR! You probably gave a wrong argument type" << endl
        << "       Bad cast: " << e.what()
        << endl;

        std::exit(-1);
    } catch (boost::exception_detail::clone_impl<
        boost::exception_detail::error_info_injector<po::invalid_option_value> > what
    ) {
        cerr
        << "ERROR: Invalid value '" << what.what() << "'" << endl
        << "       given to option '" << what.get_option_name() << "'"
        << endl;

        std::exit(-1);
    } catch (boost::exception_detail::clone_impl<
        boost::exception_detail::error_info_injector<po::multiple_occurrences> > what
    ) {
        cerr
        << "ERROR: " << what.what() << " of option '"
        << what.get_option_name() << "'"
        << endl;

        std::exit(-1);
    } catch (boost::exception_detail::clone_impl<
        boost::exception_detail::error_info_injector<po::required_option> > what
    ) {
        cerr
        << "ERROR: You forgot to give a required option '"
        << what.get_option_name() << "'"
        << endl;

        std::exit(-1);
    } catch (boost::exception_detail::clone_impl<
        boost::exception_detail::error_info_injector<po::too_many_positional_options_error> > what
    ) {
        cerr
        << "ERROR: You gave too many positional arguments. Only at most two can be given:" << endl
        << "       the 1st the CNF file input, and optinally, the 2nd the DRUP file output" << endl
        << "    OR (pre-processing)  1st for the input CNF, 2nd for the simplified CNF" << endl
        << "    OR (post-processing) 1st for the solution file" << endl
        ;

        std::exit(-1);
    } catch (boost::exception_detail::clone_impl<
        boost::exception_detail::error_info_injector<po::ambiguous_option> > what
    ) {
        cerr
        << "ERROR: The option you gave was not fully written and matches" << endl
        << "       more than one option. Please give the full option name." << endl
        << "       The option you gave: '" << what.get_option_name() << "'" <<endl
        << "       The alternatives are: ";
        for(size_t i = 0; i < what.alternatives().size(); i++) {
            cout << what.alternatives()[i];
            if (i+1 < what.alternatives().size()) {
                cout << ", ";
            }
        }
        cout << endl;

        std::exit(-1);
    } catch (boost::exception_detail::clone_impl<
        boost::exception_detail::error_info_injector<po::invalid_command_line_syntax> > what
    ) {
        cerr
        << "ERROR: The option you gave is missing the argument or the" << endl
        << "       argument is given with space between the equal sign." << endl
        << "       detailed error message: " << what.what() << endl
        ;
        std::exit(-1);
    }
}

void Main::handle_drup_option()
{
    if (drupDebug) {
        drupf = &cout;
    } else {
        std::ofstream* drupfTmp = new std::ofstream;
        drupfTmp->open(drupfilname.c_str(), std::ofstream::out);
        if (!*drupfTmp) {
            std::cerr
            << "ERROR: Could not open DRUP file "
            << drupfilname
            << " for writing"
            << endl;

            std::exit(-1);
        }
        drupf = drupfTmp;
    }

    if (!conf.otfHyperbin) {
        if (conf.verbosity >= 2) {
            cout
            << "c OTF hyper-bin is needed for BProp in DRUP, turning it back"
            << endl;
        }
        conf.otfHyperbin = true;
    }

    if (conf.doFindXors) {
        if (conf.verbosity >= 2) {
            cout
            << "c XOR manipulation is not supported in DRUP, turning it off"
            << endl;
        }
        conf.doFindXors = false;
    }

    if (conf.doRenumberVars) {
        if (conf.verbosity >= 2) {
            cout
            << "c Variable renumbering is not supported during DRUP, turning it off"
            << endl;
        }
        conf.doRenumberVars = false;
    }

    if (conf.doCompHandler) {
        if (conf.verbosity >= 2) {
            cout
            << "c Component finding & solving is not supported during DRUP, turning it off"
            << endl;
        }
        conf.doCompHandler = false;
    }
}

void Main::parse_var_elim_strategy()
{
    if (var_elim_strategy == getNameOfElimStrategy(ElimStrategy::heuristic)) {
        conf.var_elim_strategy = ElimStrategy::heuristic;
    } else if (var_elim_strategy == getNameOfElimStrategy(ElimStrategy::calculate_exactly)) {
        conf.var_elim_strategy = ElimStrategy::calculate_exactly;
    } else {
        std::cerr
        << "ERROR: Cannot parse option given to '--elimstrgy'. It's '"
        << var_elim_strategy << "'" << " but that none of the possiblities listed."
        << endl;

        std::exit(-1);
    }
}

void Main::parse_restart_type()
{
    if (vm.count("restart")) {
        string type = vm["restart"].as<string>();
        if (type == "geom")
            conf.restartType = Restart::geom;
        else if (type == "luby")
            conf.restartType = Restart::luby;
        else if (type == "glue")
            conf.restartType = Restart::glue;
        else throw WrongParam("restart", "unknown restart type");
    }
}

void Main::parse_polarity_type()
{
    if (vm.count("polar")) {
        string mode = vm["polar"].as<string>();

        if (mode == "true") conf.polarity_mode = PolarityMode::polarmode_pos;
        else if (mode == "false") conf.polarity_mode = PolarityMode::polarmode_neg;
        else if (mode == "rnd") conf.polarity_mode = PolarityMode::polarmode_rnd;
        else if (mode == "auto") conf.polarity_mode = PolarityMode::polarmode_automatic;
        else throw WrongParam(mode, "unknown polarity-mode");
    }
}

void Main::manually_parse_some_options()
{
    if (conf.shortTermHistorySize <= 0) {
        cout
        << "You MUST give a short term history size (\"--gluehist\")" << endl
        << "  greater than 0!"
        << endl;

        std::exit(-1);
    }

    if (conf.preprocess != 0) {
        conf.varelim_time_limitM *= 3;
        conf.global_timeout_multiplier *= 1.5;
        if (conf.doCompHandler) {
            conf.doCompHandler = false;
            cout << "c Cannot handle components when preprocessing. Turning it off." << endl;
        }

        if (num_threads > 1) {
            num_threads = 1;
            cout << "c Cannot handle multiple threads for preprocessing. Setting to 1." << endl;
        }


        if (!redDumpFname.empty()
            || !irredDumpFname.empty()
        ) {
            std::cerr << "ERROR: dumping clauses with preprocessing makes no sense. Exiting" << endl;
            std::exit(-1);
        }

        if (max_nr_of_solutions > 1) {
            std::cerr << "ERROR: multi-solutions make no sense with preprocessing. Exiting." << endl;
            std::exit(-1);
        }

        if (!filesToRead.empty()) {
            std::cerr << "ERROR: reading in CNF file(s) make no sense with preprocessing. Exiting." << endl;
            std::exit(-1);
        }

        if (!debugLib.empty()) {
            std::cerr << "ERROR: debugLib makes no sense with preprocessing. Exiting." << endl;
            std::exit(-1);
        }

        if (vm.count("schedule")) {
            std::cerr << "ERROR: Pleaase adjust the --preschedule not the --schedule when preprocessing. Exiting." << endl;
            std::exit(-1);
        }

        if (vm.count("occschedule")) {
            std::cerr << "ERROR: Pleaase adjust the --preoccschedule not the --occschedule when preprocessing. Exiting." << endl;
            std::exit(-1);
        }

        if (!vm.count("preschedule")) {
            conf.simplify_schedule_startup = conf.simplify_schedule_preproc;
        }

        if (!vm.count("eratio")) {
            conf.varElimRatioPerIter = 1.0;
        }
    }

    if (vm.count("dumpresult")) {
        resultfile = new std::ofstream;
        resultfile->open(resultFilename.c_str());
        if (!(*resultfile)) {
            cout
            << "ERROR: Couldn't open file '"
            << resultFilename
            << "' for writing!"
            << endl;
            std::exit(-1);
        }
    }

    parse_polarity_type();

    if (conf.random_var_freq < 0 || conf.random_var_freq > 1) {
        throw WrongParam(lexical_cast<string>(conf.random_var_freq), "Illegal random var frequency ");
    }

    //Conflict
    if (vm.count("maxdump") && redDumpFname.empty()) {
        throw WrongParam("maxdump", "--dumpred <filename> must be activated if issuing --maxdump <size>");
    }

    parse_restart_type();
    parse_var_elim_strategy();

    if (conf.preprocess == 2) {
        if (vm.count("input") == 0) {
            cout << "ERROR: When post-processing you must give the solution as the positional argument"
            << endl;
            std::exit(-1);
        }

        vector<string> solution = vm["input"].as<vector<string> >();
        if (solution.size() > 1) {
            cout << "ERROR: When post-processing you must give only the solution as the positional argument"
            << endl;
            std::exit(-1);
        }
        conf.solution_file = solution[0];
    } else if (vm.count("input")) {
        filesToRead = vm["input"].as<vector<string> >();
        if (!vm.count("sqlitedb")) {
            conf.sqlite_filename = filesToRead[0] + ".sqlite";
        } else {
            conf.sqlite_filename = vm["sqlitedb"].as<string>();
        }
        fileNamePresent = true;
    } else {
        fileNamePresent = false;
    }

    if (conf.preprocess == 1) {
        if (!vm.count("drup")) {
            cout << "ERROR: When preprocessing, you must give the simplified file name as 2nd argument" << endl;
            std::exit(-1);
        }
        conf.simplified_cnf = vm["drup"].as<string>();
    }

    if (conf.preprocess == 2) {
        if (vm.count("drup")) {
            cout << "ERROR: When postprocessing, you must NOT give a 2nd argument" << endl;
            std::exit(-1);
        }
    }

    if (conf.preprocess == 0 && vm.count("drup")) {
        handle_drup_option();
    }

    if (conf.verbosity >= 1) {
        cout << "c Outputting solution to console" << endl;
    }
}

void Main::parseCommandLine()
{
    clear_interrupt = 0;
    conf.verbosity = 2;
    conf.verbStats = 1;

    //Reconstruct the command line so we can emit it later if needed
    for(int i = 0; i < argc; i++) {
        commandLine += string(argv[i]);
        if (i+1 < argc) {
            commandLine += " ";
        }
    }

    add_supported_options();
    check_options_correctness();
    if (vm.count("version")) {
        printVersionInfo();
        std::exit(0);
    }

    try {
        manually_parse_some_options();
    } catch(WrongParam& p) {
        cerr << "ERROR: " << p.getMsg() << endl;
        exit(-1);
    }
}

void Main::printVersionInfo()
{
    cout << "c CryptoMiniSat version " << solver->get_version() << endl;
    cout << "c CryptoMiniSat SHA revision " << solver->get_version_sha1() << endl;
    cout << "c CryptoMiniSat compilation env " << solver->get_compilation_env() << endl;
    #ifdef __GNUC__
    cout << "c compiled with gcc version " << __VERSION__ << endl;
    #else
    cout << "c compiled with non-gcc compiler" << endl;
    #endif
}

void Main::dumpIfNeeded() const
{
    if (redDumpFname.empty()
        && irredDumpFname.empty()
    ) {
        return;
    }

    if (!redDumpFname.empty()) {
        solver->open_file_and_dump_red_clauses(redDumpFname);
        if (conf.verbosity >= 1) {
            cout << "c Dumped redundant clauses" << endl;
        }
    }

    if (!irredDumpFname.empty()) {
        solver->open_file_and_dump_irred_clauses(irredDumpFname);
        if (conf.verbosity >= 1) {
            cout
            << "c [solver] Dumped irredundant clauses to file "
            << "'" << irredDumpFname << "'." << endl
            << "c [solver] Note that these may NOT be in the original CNF, but"
            << " *describe the same problem* with the *same variables*"
            << endl;
        }
    }
}

void Main::check_num_threads_sanity(const unsigned thread_num) const
{
    #ifdef USE_PTHREADS
    const unsigned num_cores = std::thread::hardware_concurrency();
    if (num_cores == 0) {
        //Library doesn't know much, we can't do any checks.
        return;
    }

    if (thread_num > num_cores) {
        std::cerr
        << "c WARNING: Number of threads requested is more than the number of"
        << " cores reported by the system.\n"
        << "c WARNING: This is not a good idea in general. It's best to set the"
        << " number of threads to the number of real cores" << endl;
    }
    #endif
}


int Main::correctReturnValue(const lbool ret) const
{
    int retval = -1;
    if (ret == l_True) {
        retval = 10;
    } else if (ret == l_False) {
        retval = 20;
    } else if (ret == l_Undef) {
        retval = 15;
    } else {
        std::cerr << "Something is very wrong, output is neither l_Undef, nor l_False, nor l_True" << endl;
        exit(-1);
    }

    if (zero_exit_status) {
        return 0;
    } else {
        return retval;
    }
}

int Main::solve()
{
    solver = new SATSolver((void*)&conf);
    solverToInterrupt = solver;
    if (drupf) {
        solver->set_drup(drupf);
    }
    check_num_threads_sanity(num_threads);
    solver->set_num_threads(num_threads);

    //Print command line used to execute the solver: for options and inputs
    if (conf.verbosity >= 1) {
        printVersionInfo();
        cout
        << "c Executed with command line: "
        << commandLine
        << endl;
    }
    solver->add_sql_tag("commandline", commandLine);

    //Parse in DIMACS (maybe gzipped) files
    //solver->log_to_file("mydump.cnf");
    if (conf.preprocess != 2) {
        parseInAllFiles(solver);
    }

    lbool ret = multi_solutions();
    dumpIfNeeded();

    if (conf.preprocess != 1) {
        if (ret == l_Undef && conf.verbosity >= 1) {
            cout
            << "c Not finished running -- signal caught or some maximum reached"
            << endl;
        }
        if (conf.verbosity >= 1) {
            solver->print_stats();
        }
    }
    printResultFunc(&cout, false, ret);
    if (resultfile) {
        printResultFunc(resultfile, true, ret);
    }

    return correctReturnValue(ret);
}

lbool Main::multi_solutions()
{
    unsigned long current_nr_of_solutions = 0;
    lbool ret = l_True;
    while(current_nr_of_solutions < max_nr_of_solutions && ret == l_True) {
        ret = solver->solve();
        current_nr_of_solutions++;

        if (ret == l_True && current_nr_of_solutions < max_nr_of_solutions) {
            printResultFunc(&cout, false, ret);
            if (resultfile) {
                printResultFunc(resultfile, true, ret);
            }

            if (conf.verbosity >= 1) {
                cout
                << "c Number of solutions found until now: "
                << std::setw(6) << current_nr_of_solutions
                << endl;
            }
            #ifdef VERBOSE_DEBUG_RECONSTRUCT
            solver->print_removed_vars();
            #endif

            //Banning found solution
            vector<Lit> lits;
            for (uint32_t var = 0; var < solver->nVars(); var++) {
                if (solver->get_model()[var] != l_Undef) {
                    lits.push_back( Lit(var, (solver->get_model()[var] == l_True)? true : false) );
                }
            }
            solver->add_clause(lits);
        }
    }
    return ret;
}

string binary(unsigned x, uint32_t length)
{
    uint32_t logSize = (x == 0 ? 1 : log2(x) + 1);
    string s;
    do {
        s.push_back('0' + (x & 1));
    } while (x >>= 1);
    for (uint32_t i = logSize; i < (uint32_t) length; i++) {
        s.push_back('0');
    }
    std::reverse(s.begin(), s.end());

    return s;

}

bool Main::GenerateRandomBits(string& randomBits, uint32_t size, std::mt19937& randomEngine)
{
    std::uniform_int_distribution<unsigned> uid {0, 2147483647};
    uint32_t i = 0;
    while (i < size) {
        i += 31;
        randomBits += binary(uid(randomEngine), 31);
    }
    return true;
}
int Main::GenerateRandomNum(int maxRange, std::mt19937& randomEngine)
{
    std::uniform_int_distribution<int> uid {0, maxRange};
    return uid(randomEngine);
}

/* Number of solutions to return from one invocation of UniGen2 */
uint32_t Main::SolutionsToReturn(
    uint32_t minSolutions
) {
    if (conf.multisample) {
        return minSolutions;
    } else {
        return 1;
    }
}
bool Main::AddHash(uint32_t numClaus, SATSolver* solver, vector<Lit>& assumptions, std::mt19937& randomEngine)
{
    string randomBits;
    GenerateRandomBits(randomBits, (independent_vars.size() + 1) * numClaus, randomEngine);
    bool rhs = true;
    uint32_t activationVar;
    vector<uint32_t> vars;

    for (uint32_t i = 0; i < numClaus; i++) {
        vars.clear();
        solver->new_var();
        activationVar = solver->nVars()-1;
        assumptions.push_back(Lit(activationVar, true));
        vars.push_back(activationVar);
        rhs = (randomBits[(independent_vars.size() + 1) * i] == 1);

        for (uint32_t j = 0; j < independent_vars.size(); j++) {
            if (randomBits[(independent_vars.size() + 1) * i + j] == '1') {
                vars.push_back(independent_vars[j]);
            }
        }
        solver->add_xor_clause(vars, rhs);
    }
    return true;
}

int32_t Main::BoundedSATCount(uint32_t maxSolutions, SATSolver* solver, vector<Lit>& assumptions)
{
    unsigned long current_nr_of_solutions = 0;
    lbool ret = l_True;
    solver->new_var();
    uint32_t activationVar = solver->nVars()-1;
    vector<Lit> allSATAssumptions(assumptions);
    allSATAssumptions.push_back(Lit(activationVar, true));

    //signal(SIGALRM, SIGALARM_handler);
    //start_timer(conf.loopTimeout);
    while (current_nr_of_solutions < maxSolutions && ret == l_True) {
        ret = solver->solve(&allSATAssumptions);
        current_nr_of_solutions++;
        if (ret == l_True && current_nr_of_solutions < maxSolutions) {
            vector<Lit> lits;
            lits.push_back(Lit(activationVar, false));
            for (uint32_t j = 0; j < independent_vars.size(); j++) {
                uint32_t var = independent_vars[j];
                if (solver->get_model()[var] != l_Undef) {
                    lits.push_back(Lit(var, (solver->get_model()[var] == l_True) ? true : false));
                }
            }
            solver->add_clause(lits);
        }
    }
    vector<Lit> cls_that_removes;
    cls_that_removes.push_back(Lit(activationVar, false));
    solver->add_clause(cls_that_removes);
    if (ret == l_Undef) {
        return -1 * current_nr_of_solutions;
    }
    return current_nr_of_solutions;
}

lbool Main::BoundedSAT(
    uint32_t maxSolutions
    , uint32_t minSolutions
    , SATSolver* solver
    , vector<Lit>& assumptions
    , std::mt19937& randomEngine
    , std::map<string, uint32_t>& solutionMap
    , uint32_t* solutionCount
) {
    unsigned long current_nr_of_solutions = 0;
    lbool ret = l_True;
    solver->new_var();
    uint32_t activationVar = solver->nVars()-1;
    vector<Lit> allSATAssumptions(assumptions);
    allSATAssumptions.push_back(Lit(activationVar, true));

    std::vector<vector<lbool>> modelsSet;
    vector<lbool> model;
    while (current_nr_of_solutions < maxSolutions && ret == l_True) {
        ret = solver->solve(&allSATAssumptions);
        current_nr_of_solutions++;

        if (ret == l_True && current_nr_of_solutions < maxSolutions) {
            vector<Lit> lits;
            lits.push_back(Lit(activationVar, false));
            model.clear();
            model = solver->get_model();
            modelsSet.push_back(model);
            for (uint32_t j = 0; j < independent_vars.size(); j++) {
                uint32_t var = independent_vars[j];
                if (solver->get_model()[var] != l_Undef) {
                    lits.push_back(Lit(var, (solver->get_model()[var] == l_True) ? true : false));
                }
            }
            solver->add_clause(lits);
        }
    }
    *solutionCount = modelsSet.size();
    cout << "current_nr_of_solutions:" << current_nr_of_solutions << endl;
    vector<Lit> cls_that_removes;
    cls_that_removes.push_back(Lit(activationVar, false));
    solver->add_clause(cls_that_removes);

    if (current_nr_of_solutions < maxSolutions && current_nr_of_solutions > minSolutions) {
        std::vector<int> modelIndices;
        for (uint32_t i = 0; i < modelsSet.size(); i++) {
            modelIndices.push_back(i);
        }
        std::shuffle(modelIndices.begin(), modelIndices.end(), randomEngine);
        uint32_t var;
        uint32_t numSolutionsToReturn = SolutionsToReturn(minSolutions);
        for (uint32_t i = 0; i < numSolutionsToReturn; i++) {
            vector<lbool> model = modelsSet.at(modelIndices.at(i));
            string solution ("v");
            for (uint32_t j = 0; j < independent_vars.size(); j++) {
                var = independent_vars[j];
                if (model[var] != l_Undef) {
                    if (model[var] != l_True) {
                        solution += "-";
                    }
                    solution += std::to_string(var + 1);
                    solution += " ";
                }
            }
            solution += "0";

            std::map<string, uint32_t>::iterator it = solutionMap.find(solution);
            if (it == solutionMap.end()) {
                solutionMap[solution] = 0;
            }
            solutionMap[solution] += 1;
        }
        return l_True;

    }

    return l_False;
}

template<class T>
inline double findMean(T numList)
{
    assert(!numList.empty());

    double sum = 0;
    for (const auto a: numList) {
        sum += a;
    }
    return (sum * 1.0 / numList.size());
}

inline double findMedian(list<int> numList)
{
    numList.sort();
    int medIndex = int((numList.size() + 1) / 2);
    list<int>::iterator it = numList.begin();
    if (medIndex >= (int) numList.size()) {
        std::advance(it, numList.size() - 1);
        return double(*it);
    }
    std::advance(it, medIndex);
    return double(*it);
}

inline int findMin(list<int> numList)
{
    int min = std::numeric_limits<int>::max();
    for (list<int>::iterator it = numList.begin(); it != numList.end(); it++) {
        if ((*it) < min) {
            min = *it;
        }
    }
    return min;
}

SATCount Main::ApproxMC(SATSolver* solver, FILE* resLog, std::mt19937& randomEngine)
{
    int32_t currentNumSolutions = 0;
    std::list<int> numHashList, numCountList;
    vector<Lit> assumptions;
    double elapsedTime = 0;
    int repeatTry = 0;
    for (uint32_t j = 0; j < conf.tApproxMC; j++) {
        uint32_t  hashCount;
        for (hashCount = 0; hashCount < solver->nVars(); hashCount++) {
            double currentTime = cpuTimeTotal();
            elapsedTime = currentTime - startTime;
            if (elapsedTime > conf.totalTimeout - 3000) {
                break;
            }
            double myTime = cpuTimeTotal();
            currentNumSolutions = BoundedSATCount(conf.pivotApproxMC + 1, solver, assumptions);

            myTime = cpuTimeTotal() - myTime;
            //printf("%f\n", myTime);
            //printf("%d %d\n",currentNumSolutions,conf.pivotApproxMC);
            if (conf.verbosity >= 2) {
                fprintf(resLog, "ApproxMC:%d:%d:%f:%d:%d\n", j, hashCount, myTime,
                        (currentNumSolutions == (int32_t)(conf.pivotApproxMC + 1)), currentNumSolutions);
                fflush(resLog);
            }
            if (currentNumSolutions <= 0) {
                assumptions.clear();
                if (repeatTry < 2) {    /* Retry up to twice more */
                    AddHash(hashCount, solver, assumptions, randomEngine);
                    hashCount --;
                    repeatTry += 1;
                } else {
                    AddHash(hashCount + 1, solver, assumptions, randomEngine);
                    repeatTry = 0;
                }
                continue;
            }
            if (currentNumSolutions == conf.pivotApproxMC + 1) {
                AddHash(1, solver, assumptions, randomEngine);
            } else {
                break;
            }

        }
        assumptions.clear();
        if (elapsedTime > conf.totalTimeout - 3000) {
            break;
        }
        numHashList.push_back(hashCount);
        numCountList.push_back(currentNumSolutions);
    }
    if (numHashList.size() == 0) {
        return SATCount();
    }
    int minHash = findMin(numHashList);
    for (std::list<int>::iterator it1 = numHashList.begin(), it2 = numCountList.begin();
            it1 != numHashList.end() && it2 != numCountList.end(); it1++, it2++) {
        (*it2) *= pow(2, (*it1) - minHash);
    }
    int medSolCount = findMedian(numCountList);

    SATCount solCount;
    solCount.cellSolCount = medSolCount;
    solCount.hashCount = minHash;
    return solCount;
}

uint32_t Main::UniGen(
    uint32_t samples
    , SATSolver* solver
    , FILE* resLog
    , uint32_t sampleCounter
    , std::mt19937& randomEngine
    , std::map<string, uint32_t>& solutionMap
    , uint32_t* lastSuccessfulHashOffset
    , double timeReference
) {
    lbool ret = l_False;
    uint32_t i, solutionCount, currentHashCount, lastHashCount, currentHashOffset, hashOffsets[3];
    int hashDelta;
    vector<Lit> assumptions;
    double elapsedTime = 0;
    int repeatTry = 0;
    for (i = 0; i < samples; i++) {
        sampleCounter ++;
        ret = l_False;

        hashOffsets[0] = *lastSuccessfulHashOffset;   /* Start at last successful hash offset */
        if (hashOffsets[0] == 0) {  /* Starting at q-2; go to q-1 then q */
            hashOffsets[1] = 1;
            hashOffsets[2] = 2;
        } else if (hashOffsets[0] == 2) { /* Starting at q; go to q-1 then q-2 */
            hashOffsets[1] = 1;
            hashOffsets[2] = 0;
        }
        repeatTry = 0;
        lastHashCount = 0;
        for (uint32_t j = 0; j < 3; j++) {
            currentHashOffset = hashOffsets[j];
            currentHashCount = currentHashOffset + conf.startIteration;
            hashDelta = currentHashCount - lastHashCount;

            if (hashDelta > 0) { /* Add new hash functions */
                AddHash(hashDelta, solver, assumptions, randomEngine);
            } else if (hashDelta < 0) { /* Remove hash functions */
                assumptions.clear();
                AddHash(currentHashCount, solver, assumptions, randomEngine);
            }
            lastHashCount = currentHashCount;

            double currentTime = cpuTimeTotal();
            elapsedTime = currentTime - startTime;
            if (elapsedTime > conf.totalTimeout - 3000) {
                break;
            }
            uint32_t maxSolutions = (uint32_t) (1.41 * (1 + conf.kappa) * conf.pivotUniGen + 2);
            uint32_t minSolutions = (uint32_t) (conf.pivotUniGen / (1.41 * (1 + conf.kappa)));
            ret = BoundedSAT(maxSolutions + 1, minSolutions, solver, assumptions, randomEngine, solutionMap, &solutionCount);
            if (conf.verbosity >= 2) {
                fprintf(resLog, "UniGen2:%d:%d:%f:%d:%d\n", sampleCounter, currentHashCount, cpuTimeTotal() - timeReference, (ret == l_False ? 1 : (ret == l_True ? 0 : 2)), solutionCount);
                fflush(resLog);
            }
            if (ret == l_Undef) {   /* SATSolver timed out; retry current hash count at most twice more */
                assumptions.clear();    /* Throw out old hash functions */
                if (repeatTry < 2) {    /* Retry current hash count with new hash functions */
                    AddHash(currentHashCount, solver, assumptions, randomEngine);
                    j--;
                    repeatTry += 1;
                } else {     /* Go on to next hash count */
                    lastHashCount = 0;
                    if ((j == 0) && (currentHashOffset == 1)) { /* At q-1, and need to pick next hash count */
                        /* Somewhat arbitrarily pick q-2 first; then q */
                        hashOffsets[1] = 0;
                        hashOffsets[2] = 2;
                    }
                    repeatTry = 0;
                }
                continue;
            }
            if (ret == l_True) {    /* Number of solutions in correct range */
                *lastSuccessfulHashOffset = currentHashOffset;
                break;
            } else { /* Number of solutions too small or too large */
                if ((j == 0) && (currentHashOffset == 1)) { /* At q-1, and need to pick next hash count */
                    if (solutionCount < minSolutions) {
                        /* Go to q-2; next will be q */
                        hashOffsets[1] = 0;
                        hashOffsets[2] = 2;
                    } else {
                        /* Go to q; next will be q-2 */
                        hashOffsets[1] = 2;
                        hashOffsets[2] = 0;
                    }
                }
            }
        }
        if (ret != l_True) {
            i --;
        }
        assumptions.clear();
        if (elapsedTime > conf.totalTimeout - 3000) {
            break;
        }
    }
    return sampleCounter;
}

int Main::singleThreadUniGenCall(
    uint32_t samples
    , FILE* resLog
    , uint32_t sampleCounter
    , std::map<string, uint32_t>& solutionMap
    , std::mt19937& randomEngine
    , uint32_t* lastSuccessfulHashOffset
    , double timeReference
) {
    SATSolver solver2(&conf);

    parseInAllFiles(&solver2);
    sampleCounter = UniGen(
        samples
        , &solver2
        , resLog
        , sampleCounter
        , randomEngine
        , solutionMap
        , lastSuccessfulHashOffset
        , timeReference
    );
    return sampleCounter;
}

void Main::SeedEngine(std::mt19937& randomEngine)
{
    /* Initialize PRNG with seed from random_device */
    std::random_device rd {};
    std::array<int, 10> seedArray;
    std::generate_n(seedArray.data(), seedArray.size(), std::ref(rd));
    std::seed_seq seed(std::begin(seedArray), std::end(seedArray));
    randomEngine.seed(seed);
}

bool Main::openLogFile(FILE*& res)
{
    if (false) {
        return false;
    }

    string suffix, logFileName;
    for (int i = 0; i < 1; i++) {
        suffix = "_";
        suffix.append(std::to_string(i).append(".txt"));
        logFileName = "mylog";
        res = fopen(logFileName.append(suffix).c_str(), "wb");
        if (res == NULL) {
            int backup_errno = errno;
            printf("Cannot open %s for writing. Problem: %s\n", logFileName.append(suffix).c_str(), strerror(backup_errno));
            exit(1);
        }
    }
    return true;
}

//My stuff from OneThreadSolve
int Main::UniSolve()
{
    conf.reconfigure_at = 0;
    conf.reconfigure_val = 7;

    FILE* resLog;
    openLogFile(resLog);
    startTime = cpuTimeTotal();

    solver = new SATSolver((void*)&conf);
    solverToInterrupt = solver;
    if (drupf) {
        solver->set_drup(drupf);
    }
    //check_num_threads_sanity(num_threads);
    //solver->set_num_threads(num_threads);
    parseInAllFiles(solver);

    if (conf.startIteration > independent_vars.size()) {
        cout << "ERROR: Manually-specified startIteration"
        "is larger than the size of the independent set.\n" << endl;
        return -1;
    }
    if (conf.startIteration == 0) {
        cout << "Computing startIteration using ApproxMC" << endl;

        SATCount solCount;
        std::mt19937 randomEngine {};
        SeedEngine(randomEngine);
        solCount = ApproxMC(solver, resLog, randomEngine);
        double elapsedTime = cpuTimeTotal() - startTime;
        cout << "Completed ApproxMC at " << elapsedTime << " s";
        if (elapsedTime > conf.totalTimeout - 3000) {
            cout << " (TIMED OUT)" << endl;
            return 0;
        }
        cout << endl;
        //printf("Solution count estimate is %d * 2^%d\n", solCount.cellSolCount, solCount.hashCount);
        if (solCount.hashCount == 0 && solCount.cellSolCount == 0) {
            cout << "The input formula is unsatisfiable." << endl;
            return 0;
        }
        conf.startIteration = round(solCount.hashCount + log2(solCount.cellSolCount) +
                                    log2(1.8) - log2(conf.pivotUniGen)) - 2;
    } else {
        cout << "Using manually-specified startIteration" << endl;
    }

    uint32_t maxSolutions = (uint32_t) (1.41 * (1 + conf.kappa) * conf.pivotUniGen + 2);
    uint32_t minSolutions = (uint32_t) (conf.pivotUniGen / (1.41 * (1 + conf.kappa)));
    uint32_t samplesPerCall = SolutionsToReturn(minSolutions);
    uint32_t callsNeeded = (conf.samples + samplesPerCall - 1) / samplesPerCall;
    printf("loThresh %d, hiThresh %d, startIteration %d\n", minSolutions, maxSolutions, conf.startIteration);
    printf("Outputting %d solutions from each UniGen2 call\n", samplesPerCall);
    uint32_t numCallsInOneLoop = 0;
    if (conf.callsPerSolver == 0) {
        numCallsInOneLoop = std::min(solver->nVars() / (conf.startIteration * 14), callsNeeded);
        if (numCallsInOneLoop == 0) {
            numCallsInOneLoop = 1;
        }
    } else {
        numCallsInOneLoop = conf.callsPerSolver;
        cout << "Using manually-specified callsPerSolver" << endl;
    }

    uint32_t numCallLoops = callsNeeded / numCallsInOneLoop;
    uint32_t remainingCalls = callsNeeded % numCallsInOneLoop;

    cout << "Making " << numCallLoops << " loops."
    << " calls per loop: " << numCallsInOneLoop
    << " remaining: " << remainingCalls << endl;
    bool timedOut = false;
    uint32_t sampleCounter = 0;
    std::map<string, uint32_t> threadSolutionMap;
    double allThreadsTime = 0;
    uint32_t allThreadsSampleCount = 0;
    double threadStartTime = cpuTimeTotal();

    std::mt19937 randomEngine {};
    SeedEngine(randomEngine);

    uint32_t lastSuccessfulHashOffset = 0;
    lbool ret = l_True;

    /* Perform extra UniGen calls that don't fit into the loops */
    if (remainingCalls > 0) {
        sampleCounter = singleThreadUniGenCall(
            remainingCalls, resLog, sampleCounter
            , threadSolutionMap, randomEngine
            , &lastSuccessfulHashOffset, threadStartTime);
    }

    /* Perform main UniGen call loops */
    for (uint32_t i = 0; i < numCallLoops; i++) {
        if (!timedOut) {
            sampleCounter = singleThreadUniGenCall(
                numCallsInOneLoop, resLog, sampleCounter, threadSolutionMap
                , randomEngine, &lastSuccessfulHashOffset, threadStartTime
            );

            if ((cpuTimeTotal() - threadStartTime) > conf.totalTimeout - 3000) {
                timedOut = true;
            }
        }
    }

    for (map<string, uint32_t>::iterator itt = threadSolutionMap.begin()
        ; itt != threadSolutionMap.end()
        ; itt++
    ) {
        string solution = itt->first;
        map<string, std::vector<uint32_t>>::iterator itg = globalSolutionMap.find(solution);
        if (itg == globalSolutionMap.end()) {
            globalSolutionMap[solution] = std::vector<uint32_t>(1, 0);
        }
        globalSolutionMap[solution][0] += itt->second;
        allThreadsSampleCount += itt->second;
    }

    double timeTaken = cpuTimeTotal() - threadStartTime;
    allThreadsTime += timeTaken;
    cout
    << "Total time for UniGen2 thread " << 1
    << ": " << timeTaken << " s"
    << (timedOut ? " (TIMED OUT)" : "")
    << endl;

    cout << "Total time for all UniGen2 calls: " << allThreadsTime << " s" << endl;
    cout << "Samples generated: " << allThreadsSampleCount << endl;

    if (conf.verbosity >= 1) {
        solver->print_stats();
    }

    return correctReturnValue(ret);
}

int main(int argc, char** argv)
{
    #if defined(__GNUC__) && defined(__linux__)
    feenableexcept(FE_INVALID   |
                   FE_DIVBYZERO |
                   FE_OVERFLOW
    );
    #endif

    Main main(argc, argv);
    main.parseCommandLine();

    signal(SIGINT, SIGINT_handler);

    if (unisolve) {
        return main.UniSolve();
    } else {
        return main.solve();
    }
}
