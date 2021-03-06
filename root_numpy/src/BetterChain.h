#ifndef __BETTER_CHAIN_H
#define __BETTER_CHAIN_H

#include <Python.h>
#include <string>
#include <iostream>
#include <TTree.h>
#include <TFile.h>
#include <TChain.h>
#include <TLeaf.h>
#include <TTreeFormula.h>
#include <TObject.h>

#include <map>
#include <vector>
#include <cassert>
#include <set>
#include <iomanip>
#include <fstream>
#include <cstdlib>
#include <cstdio>

#include "Column.h"
#include "util.h"

using namespace std;

// Correct TChain implementation with cache TLeaf*
class BetterChain
{
    public:

        BetterChain(TTree* fChain):
            fChain(fChain),
            ientry(0)
        {
            fCurrent = -1;
            notifier = new MiniNotify(fChain->GetNotify());
            fChain->SetNotify(notifier);
            LoadTree(0);

            // Remember original branch status
            TObjArray* branches = fChain->GetListOfBranches();
            int ibranch, nbranches;
            TBranch* branch;
            nbranches = branches->GetEntries();
            for (ibranch = 0; ibranch < nbranches; ++ibranch)
            {
                branch = (TBranch*) branches->At(ibranch);
                original_branch_status[branch->GetName()] =
                    branch->TestBit(kDoNotProcess) == 0;
                // Only the required branches will be added to the cache later
                fChain->DropBranchFromCache(branch, kTRUE);
            }
            // Enable all branches since we don't know yet which branches are
            // required by the selection expression. All branches will be
            // disabled in InitBranches() before only enabling the ones that are
            // actually required in InitBranches() and MakeColumn()
            fChain->SetBranchStatus("*", 1);
            //fChain->SetCacheSize(10000000);
        }

        ~BetterChain()
        {
            if (!fChain)
                return; // Somehow i need this (copy from make class)

            // Revert branches to their original activated/deactivated state
            map<string, bool>::iterator status_it;
            for (status_it = original_branch_status.begin();
                 status_it != original_branch_status.end();
                 ++status_it)
            {
                fChain->SetBranchStatus(status_it->first.c_str(),
                                        status_it->second);
            }

            fChain->SetNotify(notifier->oldnotify); // Do not switch these two lines!
            //delete fChain->GetCurrentFile(); // ROOT does something funny

            LeafCache::iterator it;
            for(it = leafcache.begin(); it != leafcache.end(); ++it)
            {
                delete it->second;
            }

            // BetterChain owns the formulae and so we delete them here
            vector<TTreeFormula*>::iterator fit;
            for (fit = formulae.begin(); fit != formulae.end(); ++fit)
            {
                delete *fit;
            }

            delete notifier;
        }

        int LoadTree(int entry)
        {
            if (!fChain)
                return -5;
            //RNHEXDEBUG(fChain->FindBranch("mcLen")->FindLeaf("mcLen"));
            Long64_t centry = fChain->LoadTree(entry);
            //RNHEXDEBUG(fChain->FindBranch("mcLen")->FindLeaf("mcLen"));
            if (centry < 0)
                return centry;
            if (fChain->GetTreeNumber() != fCurrent)
            {
                fCurrent = fChain->GetTreeNumber();
            }
            if(notifier->notified)
            {
                Notify();
                notifier->notified=false;
            }
            return centry;
        }

        void AddFormula(TTreeFormula* formula)
        {
            // The BetterChain will take ownership of the formula
            if (formula == NULL)
                return;
            formulae.push_back(formula);
        }

        void InitBranches()
        {
            // Call this after all formulae have been defined but before
            // MakeColumn. The branches must be activated when a
            // TTreeFormula is initially created.

            // Disable all branches
            fChain->SetBranchStatus("*", 0);

            // Activate all branches used by the formulae
            int ncodes;
            TBranch* branch;
            vector<TTreeFormula*>::iterator fit;
            for (fit = formulae.begin(); fit != formulae.end(); ++fit)
            {
                ncodes = (*fit)->GetNcodes();
                for (int n = 0; n < ncodes; ++n)
                {
                    branch = (*fit)->GetLeaf(n)->GetBranch();
                    // Make the branch active and cache it
                    fChain->SetBranchStatus(branch->GetName(), 1);
                    fChain->AddBranchToCache(branch, kTRUE);
                }
            }
        }

        int GetEntry(int entry)
        {
            // Read contents of entry.
            if (!fChain)
                return 0;
            LoadTree(entry);
            ientry = entry;
            return fChain->GetEntry(ientry);
        }

        int Next()
        {
            int ret = GetEntry(ientry);
            ++ientry;
            return ret;
        }

        void Notify()
        {
            // Taking care of all the leaves
            //RNDEBUG("NOTIFY");
            LeafCache::iterator it;
            for(it = leafcache.begin(); it != leafcache.end(); ++it)
            {
                string bname = it->first.first;
                string lname = it->first.second;
                TBranch* branch = fChain->FindBranch(bname.c_str());
                if (branch==0)
                {
                    cerr << "Warning cannot find branch " << bname << endl;
                    it->second->skipped = true;
                    continue;
                }
                TLeaf* leaf = branch->FindLeaf(lname.c_str());
                if (leaf==0)
                {
                    cerr << "Warning cannot find leaf " << lname
                         << " for branch " << bname << endl;
                    it->second->skipped = true;
                    continue;
                }
                it->second->SetLeaf(leaf, true);
                it->second->skipped = false;
            }

            // Update all formula leaves
            vector<TTreeFormula*>::iterator fit;
            for (fit = formulae.begin(); fit != formulae.end(); ++fit)
            {
                (*fit)->UpdateFormulaLeaves();
            }
        }

        int GetEntries()
        {
            return fChain->GetEntries();
        }

        TBranch* FindBranch(const char* bname)
        {
            return fChain->FindBranch(bname);
        }

        Column* MakeColumn(const string& bname,
                           const string& lname,
                           const string& colname)
        {
            // As a bonus set branch status on all the active branch
            // including the branch that define the length
            LoadTree(0);

            TBranch* branch = fChain->FindBranch(bname.c_str());
            if (branch == 0)
            {
                PyErr_SetString(PyExc_IOError,
                    format("Cannot find branch %s", bname.c_str()).c_str());
                return 0;
            }

            TLeaf* leaf = fChain->FindLeaf(lname.c_str());
            if (leaf==0)
            {
                PyErr_SetString(PyExc_IOError,
                    format("Cannot find leaf %s for branch %s", lname.c_str(),
                           bname.c_str()).c_str());
                return 0;
            }

            // Make the branch active and cache it
            fChain->SetBranchStatus(bname.c_str(), 1);
            fChain->AddBranchToCache(branch, kTRUE);
            // and the length leaf as well

            //TODO Does it work if user doesn't want the length column in the structure?
            TLeaf* leafCount = leaf->GetLeafCount();
            if (leafCount != NULL)
            {
                fChain->SetBranchStatus(leafCount->GetBranch()->GetName(), 1);
                fChain->AddBranchToCache(leafCount->GetBranch(), kTRUE);
            }

            BL bl = make_pair(bname,lname);
            Column* ret = Column::build(leaf, colname);
            if (ret == 0)
                return 0;
            leafcache.insert(make_pair(bl, ret));
            return ret;
        }

        class MiniNotify: public TObject
        {
            public:
                MiniNotify(TObject* oldnotify):
                    TObject(),
                    notified(false),
                    oldnotify(oldnotify){}

                virtual Bool_t Notify()
                {
                    notified = true;
                    if (oldnotify)
                        oldnotify->Notify();
                    return true;
                }

                bool notified;
                TObject* oldnotify;
        };

        TTree* fChain;
        int fCurrent;
        int ientry;
        MiniNotify* notifier;
        vector<TTreeFormula*> formulae;
        map<string, bool> original_branch_status;

        // Branch name to leaf name conversion
        typedef pair<string, string> BL;
        typedef map<BL, Column*> LeafCache;

        // Column pointer cache since the leaf inside needs to be updated
        // when new file is loaded in the chain
        LeafCache leafcache;
};

#endif
