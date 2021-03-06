// Author: Enrico Guiraud, Danilo Piparo CERN  03/2017

/*************************************************************************
 * Copyright (C) 1995-2016, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include <stdexcept>
#include <string>
#include <typeinfo>

#include "RConfigure.h"      // R__USE_IMT
#include "ROOT/TDFNodes.hxx" // ColumnName2ColumnTypeName -> TCustomColumnBase, FindUnknownColumns -> TLoopManager
#include "ROOT/TDataSource.hxx"
#include "RtypesCore.h"
#include "TBranch.h"
#include "TBranchElement.h"
#include "TClass.h"
#include "TClassEdit.h"
#include "TClassRef.h"
#include "TLeaf.h"
#include "TObjArray.h"
#include "TROOT.h" // IsImplicitMTEnabled, GetImplicitMTPoolSize
#include "TTree.h"

using namespace ROOT::Detail::TDF;
using namespace ROOT::Experimental::TDF;

namespace ROOT {
namespace Internal {
namespace TDF {

/// Return the type_info associated to a name. If the association fails, an
/// exception is thrown.
/// References and pointers are not supported since those cannot be stored in
/// columns.
const std::type_info &TypeName2TypeID(const std::string &name)
{
   if (auto c = TClass::GetClass(name.c_str())) {
      return *c->GetTypeInfo();
   } else if (name == "char" || name == "Char_t")
      return typeid(char);
   else if (name == "unsigned char" || name == "UChar_t")
      return typeid(unsigned char);
   else if (name == "int" || name == "Int_t")
      return typeid(int);
   else if (name == "unsigned int" || name == "UInt_t")
      return typeid(unsigned int);
   else if (name == "short" || name == "Short_t")
      return typeid(short);
   else if (name == "unsigned short" || name == "UShort_t")
      return typeid(unsigned short);
   else if (name == "long" || name == "Long_t")
      return typeid(long);
   else if (name == "unsigned long" || name == "ULong_t")
      return typeid(unsigned long);
   else if (name == "double" || name == "Double_t")
      return typeid(double);
   else if (name == "float" || name == "Float_t")
      return typeid(float);
   else if (name == "long long" || name == "long long int" || name == "Long64_t")
      return typeid(Long64_t);
   else if (name == "unsigned long long" || name == "unsigned long long int" || name == "ULong64_t")
      return typeid(ULong64_t);
   else if (name == "bool" || name == "Bool_t")
      return typeid(bool);
   else {
      std::string msg("Cannot extract type_info of type ");
      msg += name.c_str();
      msg += ".";
      throw std::runtime_error(msg);
   }
}

/// Returns the name of a type starting from its type_info
/// An empty string is returned in case of failure
/// References and pointers are not supported since those cannot be stored in
/// columns.
std::string TypeID2TypeName(const std::type_info &id)
{
   if (auto c = TClass::GetClass(id)) {
      return c->GetName();
   } else if (id == typeid(char))
      return "char";
   else if (id == typeid(unsigned char))
      return "unsigned char";
   else if (id == typeid(int))
      return "int";
   else if (id == typeid(unsigned int))
      return "unsigned int";
   else if (id == typeid(short))
      return "short";
   else if (id == typeid(unsigned short))
      return "unsigned short";
   else if (id == typeid(long))
      return "long";
   else if (id == typeid(unsigned long))
      return "unsigned long";
   else if (id == typeid(double))
      return "double";
   else if (id == typeid(float))
      return "float";
   else if (id == typeid(Long64_t))
      return "Long64_t";
   else if (id == typeid(ULong64_t))
      return "ULong64_t";
   else if (id == typeid(bool))
      return "bool";
   else
      return "";
}

/// Return a string containing the type of the given branch. Works both with real TTree branches and with temporary
/// column created by Define. Returns an empty string if type name deduction fails.
/// Note that for fixed- or variable-sized c-style arrays the returned type name will be TVec<T>.
/// If the extra conversions are enabled, TVec<T> will be the type name returned also for the following types:
/// - std::vector<T>
std::string
ColumnName2ColumnTypeName(const std::string &colName, TTree *tree, TCustomColumnBase *tmpBranch, TDataSource *ds, bool extraConversions)
{
   std::string colType;

   // if this is a TDataSource column, we just ask the type name to the data-source
   if (ds && ds->HasColumn(colName))
      colType = ds->GetTypeName(colName);

   TBranch *branch = nullptr;
   if (tree) {
      // We try to get a leaf with this name. If we have one, we can get its type name and return
      const auto dotPos = colName.rfind('.');
      if (std::string::npos != dotPos && dotPos != (colName.size() - 1)) {
         TLeaf *leaf = nullptr;
         // Case 1: data members
         if ((leaf = tree->GetLeaf(colName.c_str()))) {
            return leaf->GetTypeName();
         }
         // Case 2: leaves
         const auto bName = colName.substr(0, dotPos);
         const auto lName = colName.substr(dotPos+1);
         if ((branch =  tree->GetBranch(bName.c_str()))) {
            if ((leaf = branch->GetLeaf(lName.c_str()))) {
               return leaf->GetTypeName();
            }
         }
      }

      // If we don't have a leaf, we take the full branch and continue
      branch = tree->GetBranch(colName.c_str());
   }

   auto ComposeTVecTypeName = [](const std::string &valueType) {
      return "ROOT::Experimental::VecOps::TVec<" + valueType + ">";
   };

   if (branch) {
      // this must be a real TTree branch
      static const TClassRef tbranchelRef("TBranchElement");
      if (branch->InheritsFrom(tbranchelRef)) {
         // this branch is not a fundamental type, we can ask for the class name
         std::string classname(static_cast<TBranchElement *>(branch)->GetClassName());
         if (extraConversions && ROOT::ESTLType::kSTLvector == TClassEdit::IsSTLCont(classname)) {
            std::vector<std::string> split;
            int dummy;
            TClassEdit::GetSplit(classname.c_str(), split, dummy);
            auto &valueType = split[1];
            colType = ComposeTVecTypeName(valueType);
         } else {
            colType = classname;
         }
      } else {
         // this branch must be a fundamental type or array thereof
         const auto listOfLeaves = branch->GetListOfLeaves();
         const auto nLeaves = listOfLeaves->GetEntries();
         if (nLeaves != 1)
            throw std::runtime_error("TTree branch " + colName + " has " + std::to_string(nLeaves) +
                                     " leaves. Only one leaf per branch is supported.");
         TLeaf *l = static_cast<TLeaf *>(listOfLeaves->UncheckedAt(0));
         const std::string branchType = l->GetTypeName();
         if (branchType.empty()) {
            throw std::runtime_error("could not deduce type of branch " + std::string(colName));
         } else if (l->GetLeafCount() != nullptr && l->GetLenStatic() == 1) {
            // this is a variable-sized array
            colType = ComposeTVecTypeName(branchType);
         } else if (l->GetLeafCount() == nullptr && l->GetLenStatic() > 1) {
            // this is a fixed-sized array (we do not differentiate between variable- and fixed-sized arrays)
            colType = ComposeTVecTypeName(branchType);
         } else if (l->GetLeafCount() == nullptr && l->GetLenStatic() == 1) {
            // this branch contains a single fundamental type
            colType = l->GetTypeName();
         } else {
            // we do not know how to deal with this branch
            throw std::runtime_error("TTree branch " + colName +
                                     " has both a leaf count and a static length. This is not supported.");
         }
      }
   }

   if (tmpBranch) {
      // this must be a temporary branch
      auto &id = tmpBranch->GetTypeId();
      auto typeName = TypeID2TypeName(id);
      if (typeName.empty()) {
         std::string msg("Cannot deduce type of temporary column ");
         msg += colName;
         msg += ". The typename is ";
         msg += id.name();
         msg += ".";
         throw std::runtime_error(msg);
      }
      colType = typeName;
   }

   if (colType.empty())
      throw std::runtime_error("Column \"" + colName + "\" is not in a file and has not been defined.");

   return colType;
}

/// Convert type name (e.g. "Float_t") to ROOT type code (e.g. 'F') -- see TBranch documentation.
/// Return a space ' ' in case no match was found.
char TypeName2ROOTTypeName(const std::string &b)
{
   if (b == "Char_t" || b == "char")
      return 'B';
   if (b == "UChar_t" || b == "unsigned char")
      return 'b';
   if (b == "Short_t" || b == "short" || b == "short int")
      return 'S';
   if (b == "UShort_t" || b == "unsigned short" || b == "unsigned short int")
      return 's';
   if (b == "Int_t" || b == "int")
      return 'I';
   if (b == "UInt_t" || b == "unsigned" || b == "unsigned int")
      return 'i';
   if (b == "Float_t" || b == "float")
      return 'F';
   if (b == "Double_t" || b == "double")
      return 'D';
   if (b == "Long64_t" || b == "long" || b == "long int")
      return 'L';
   if (b == "ULong64_t" || b == "unsigned long" || b == "unsigned long int")
      return 'l';
   if (b == "Bool_t" || b == "bool")
      return 'O';
   return ' ';
}

unsigned int GetNSlots()
{
   unsigned int nSlots = 1;
#ifdef R__USE_IMT
   if (ROOT::IsImplicitMTEnabled())
      nSlots = ROOT::GetImplicitMTPoolSize();
#endif // R__USE_IMT
   return nSlots;
}

/// Replace all occurrences of '.' with '_' in each string passed as argument.
/// An Info message is printed when this happens.
/// An exception is thrown in case the resulting set of strings would contain duplicates.
std::vector<std::string> ReplaceDotWithUnderscore(const std::vector<std::string> &columnNames)
{
   auto newColNames = columnNames;
   for (auto &col : newColNames) {
      if (col.find('.') != std::string::npos) {
         auto oldName = col;
         std::replace(col.begin(), col.end(), '.', '_');
         if (std::find(columnNames.begin(), columnNames.end(), col) != columnNames.end())
            throw std::runtime_error("Column " + oldName + " would be written as " + col +
                                     " but this column already exists. Please use Alias to select a new name for " +
                                     oldName);
         Info("Snapshot", "Column %s will be saved as %s", oldName.c_str(), col.c_str());
      }
   }

  return newColNames;
}

} // end NS TDF
} // end NS Internal
} // end NS ROOT
