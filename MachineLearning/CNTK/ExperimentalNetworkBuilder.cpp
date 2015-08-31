// ExperimentalNetworkBuilder.cpp -- interface to new version of NDL (and config) parser  --fseide

#define _CRT_NONSTDC_NO_DEPRECATE   // make VS accept POSIX functions without _
#define _CRT_SECURE_NO_WARNINGS     // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "Basics.h"
#include "ExperimentalNetworkBuilder.h"
#include "BrainScriptEvaluator.h"

#include "ComputationNode.h"
#include "ComputationNetwork.h"

#include <memory>
#include <deque>
#include <set>
#include <string>

#ifndef let
#define let const auto
#endif

namespace Microsoft { namespace MSR { namespace BS {

    using namespace Microsoft::MSR;

    wstring standardFunctions =
        L"Print(value, format='') = new PrintAction [ what = value /*; how = format*/ ] \n"
        L"Format(value, format) = new StringFunction [ what = 'Format' ; arg = value ; how = format ] \n"
        L"Replace(s, from, to) = new StringFunction [ what = 'Replace' ; arg = s ; replacewhat = from ; withwhat = to ] \n"
        L"Substr(s, begin, num) = new StringFunction [ what = 'Substr' ; arg = s ; pos = begin ; chars = num ] \n"
        L"Chr(c) = new StringFunction [ what = 'Chr' ;  arg = c ] \n"
        L"Floor(x)  = new NumericFunction [ what = 'Floor' ;  arg = x ] \n"
        L"Length(x) = new NumericFunction [ what = 'Length' ; arg = x ] \n"
        L"Ceil(x) = -Floor(-x) \n"
        L"Round(x) = Floor(x+0.5) \n"
        L"Abs(x) = if x >= 0 then x else -x \n"
        L"Sign(x) = if x > 0 then 1 else if x < 0 then -1 else 0 \n"
        L"Min(a,b) = if a < b then a else b \n"
        L"Max(a,b) = if a > b then a else b \n"
        L"Fac(n) = if n > 1 then Fac(n-1) * n else 1 \n"
        ;

    wstring computationNodes =
        L"Parameter(rows, cols, needGradient = true, init = 'uniform'/*|fixedValue|gaussian|fromFile*/, initValueScale = 1, value = 0, initFromFilePath = '', tag='') = new ComputationNode [ operation = 'LearnableParameter' /*plus the function args*/ ]\n"
        L"Input(rows, cols, tag='feature') = new ComputationNode [ operation = 'Input' /*plus the function args*/ ]\n"
        // ^^ already works; vv not yet working
        L"Mean(z, tag='') = new ComputationNode [ operation = 'Mean' ; inputs = z /* ; tag = tag */ ]\n"
        L"InvStdDev(z, tag='') = new ComputationNode [ operation = 'InvStdDev' ; inputs = z /* ; tag = tag */ ]\n"
        L"PerDimMeanVarNormalization(feat,mean,invStdDev, tag='') = new ComputationNode [ operation = 'PerDimMeanVarNormalization' ; inputs = feat:mean:invStdDev /* ; tag = tag */ ]\n"
        L"RowSlice(firstRow, rows, features, tag='') = new ComputationNode [ operation = 'RowSlice' ; inputs = features ; first = firstRow ; num = rows /* ; tag = tag */ ]\n"
        L"Delay(in, delay, tag='') = new ComputationNode [ operation = 'Delay' ; input = in ; deltaT = -delay /* ; tag = tag */ ]\n"
        // standard nodes, tested
        // standard nodes, untested
        L"Sigmoid(z, tag='') = new ComputationNode [ operation = 'Sigmoid' ; inputs = z /* ; tag = tag */ ]\n"
        L"Log(z, tag='') = new ComputationNode [ operation = 'Log' ; inputs = z /* ; tag = tag */ ]\n"
        L"CrossEntropyWithSoftmax(labels, outZ, tag='criterion') = new ComputationNode [ operation = 'CrossEntropyWithSoftmax' ; inputs = labels:outZ ]\n"
        L"ErrorPrediction(labels, outZ, tag='') = new ComputationNode [ operation = 'ErrorPrediction' ; inputs = labels:outZ /* ; tag = tag */ ]\n"
        ;

    wstring commonMacros =
        L"BFF(in, rows, cols) = [ B = Parameter(rows, 1, init = 'fixedValue', value = 0) ; W = Parameter(rows, cols) ; z = W*in+B ] \n"
        L"SBFF(in, rows, cols) = [ Eh = Sigmoid(BFF(in, rows, cols).z) ] \n "
        L"MeanVarNorm(feat) = PerDimMeanVarNormalization(feat, Mean(feat), InvStdDev(feat)) \n"
        L"LogPrior(labels) = Log(Mean(labels)) \n"
        ;

    // TODO: must be moved to ComputationNode.h
    // a ComputationNode that derives from MustFinalizeInit does not resolve some args immediately (just keeps ConfigValuePtrs),
    // assuming they are not ready during construction.
    // This is specifically meant to be used by DelayNode, see comments there.
    struct MustFinalizeInit { virtual void FinalizeInit() = 0; };   // derive from this to indicate ComputationNetwork should call FinalizeIitlate initialization

    template<typename ElemType>
    struct DualPrecisionHelpers
    {
        typedef shared_ptr<ComputationNode<ElemType>> ComputationNodePtr;

        // basic function template, for classes that can instantiate themselves from IConfigRecordPtr
        // TODO: do we even have any?
        template<class C>
        static shared_ptr<Object> MakeRuntimeObject(const IConfigRecordPtr config)
        {
            return make_shared<C>(config);
        }

        // -------------------------------------------------------------------
        // ComputationNetwork
        // -------------------------------------------------------------------

        // initialize a ComputationNetwork<ElemType> from a ConfigRecord
        template<>
        static shared_ptr<Object> MakeRuntimeObject<ComputationNetwork<ElemType>>(const IConfigRecordPtr configp)
        {
            let & config = *configp;

            DEVICEID_TYPE deviceId = (DEVICEID_TYPE)(int)config[L"deviceId"];
            auto net = make_shared<ComputationNetwork<ElemType>>(deviceId);

            auto & m_nameToNodeMap = net->GetNameToNodeMap();

            deque<ComputationNodePtr> workList;
            // flatten the set of all nodes
            // we collect all root ComputationNodes from the config record, and then expand into all their children by work-list processing
            // TODO: This currently only collects nodes of the same ElemType. We could allow conversion operators.
            // TODO: Can we even make the ComputationNetwork independent of ElemType?? As long as the nodes themselves are hooked up properly that should be OK!
            for (let & id : config.GetMemberIds())
            {
                let & value = config[id];
                if (value.Is<ComputationNode<ElemType>>())
                    workList.push_back((ComputationNodePtr)value);
            }
            // process work list
            // Also call FinalizeInit where we must.
            while (!workList.empty())
            {
                let node = workList.front();
                workList.pop_front();

                // add to set
                let res = m_nameToNodeMap.insert(make_pair(node->NodeName(), node));
                if (!res.second)        // not inserted: we already got this one
                    if (res.first->second == node)
                        continue;       // the same
                    else                // oops, a different node with the same name
                        LogicError("ComputationNetwork: multiple nodes with the same NodeName() '%ls'", node->NodeName().c_str());

                // If node derives from MustFinalizeInit() then it has unresolved inputs. Resolve them now.
                // This may generate a whole new load of nodes, including nodes which in turn have late init.
                // TODO: think this through whether it may generate circular references nevertheless
                let mustFinalizeInit = dynamic_pointer_cast<MustFinalizeInit>(node);
                if (mustFinalizeInit)
                    mustFinalizeInit->FinalizeInit();

                // add it to the respective node group based on the tag
                let nodeWithTag = dynamic_pointer_cast<WithTag>(node);
                if (nodeWithTag)
                {
                    wstring tag = nodeWithTag->GetTag();
                    if (tag == L"feature")                              net->FeatureNodes().push_back(node);
                    else if (tag == L"label")                           net->LabelNodes().push_back(node);
                    else if (tag == L"criterion" || tag == L"criteria") net->FinalCriterionNodes().push_back(node); // 'criteria' is wrong (plural); we keep it for compat
                    else if (!_wcsnicmp(tag.c_str(), L"eval", 4))       net->EvaluationNodes().push_back(node);     // eval*
                    else if (tag == L"output")                          net->OutputNodes().push_back(node);
                    else if (tag == L"pair")                            net->PairNodes().push_back(node);           // TODO: I made this up; the original code in SynchronousExecutionEngine did not have this
                    else if (tag == L"multiseq")                        net->NodesReqMultiSeqHandling().push_back(node);
                    else if (!tag.empty())
                        RuntimeError("ComputationNetwork: unknown tag '%ls'", tag.c_str());
                    // TODO: are there nodes without tag? Where do they go?
                }

                // TODO: ...can we do stuff like propagating dimensions here? Or still too early?

                // traverse children: append them to the end of the work list
                let children = node->GetChildren();
                for (auto child : children)
                    workList.push_back(child);  // (we could check whether c is in 'nodes' already here to optimize, but this way it is cleaner)
            }

            // TODO: what is missing is the dimensions
#if 1
            wstring args = net->ToString();
            fprintf(stderr, "%ls\n", args.c_str());
#endif
            return net;
        }

        // -------------------------------------------------------------------
        // ComputationNode -- covers all standard nodes
        // -------------------------------------------------------------------

    private:
        // helper for the factory function for ComputationNodes
        static vector<ComputationNodePtr> GetInputs(const IConfigRecord & config)
        {
            vector<ComputationNodePtr> inputs;
            let inputsArg = config[L"inputs"];
            if (inputsArg.Is<ComputationNode<ElemType>>())          // single arg
                inputs.push_back(inputsArg);
            else                                                    // a whole vector
            {
                let inputsArray = (ConfigArrayPtr)inputsArg;
                let range = inputsArray->GetIndexRange();
                for (int i = range.first; i <= range.second; i++)   // pull them. This will resolve all of them.
                    inputs.push_back(inputsArray->At(i, inputsArg.GetLocation()));
            }
            return inputs;
        }
    public:
        // create ComputationNode
        // This is the equivalent of the old SynchronousNodeEvaluator::Evaluate(), and we duplicate code from there.
        template<>
        static shared_ptr<Object> MakeRuntimeObject<ComputationNode<ElemType>>(const IConfigRecordPtr configp)
        {
            let & config = *configp;
            wstring operationName = config[L"operation"];
            wstring nodeName = L"<placeholder>";   // name will be overwritten by caller upon return (TODO: fix this here? pass expression name in?)
            DEVICEID_TYPE deviceId = (DEVICEID_TYPE)(int)config[L"deviceId"];
            static unsigned long m_randomSeedOffset = 0;    // TODO: this is held in the ComputationNetwork, but we don't have one yet

            /*  from SynchronousNodeEvaluator::Evaluate()
            if (InputValue<ElemType>::TypeName() == cnoperationName)
            else if (InputValue<ElemType>::SparseTypeName() == cnNodeType)
            else if (cnNodeType == L"ImageInput")
            else if (cnNodeType == L"SparseImageInput")
            else if (LearnableParameter<ElemType>::TypeName() == cnNodeType)
            else if (SparseLearnableParameter<ElemType>::TypeName() == cnNodeType)
            else if (cnNodeType == L"Constant")
            else if (cnNodeType == RowSliceNode<ElemType>::TypeName())
            else if (cnNodeType == RowRepeatNode<ElemType>::TypeName())
            else if (cnNodeType == ReshapeNode<ElemType>::TypeName())
            else if (cnNodeType == PastValueNode<ElemType>::TypeName() ||
                cnNodeType == FutureValueNode<ElemType>::TypeName())
            else if (cnNodeType == ConvolutionNode<ElemType>::TypeName())
            else if (cnNodeType == MaxPoolingNode<ElemType>::TypeName())
            else if (cnNodeType == AveragePoolingNode<ElemType>::TypeName())
            */

            // note on optional parameters
            // Instead of defining optional parameters here in code, they are defined as optional args to the creating macro.

            ComputationNodePtr node;
            // first group: nodes without inputs
            if (operationName == L"LearnableParameter")
            {
                // parameters[rows, [cols=1]] plus other optional parameters (needGradient=[true|false], init=[uniform|gaussian|fixedvalue], initValueScale=[1|float], value=[0|float])
                // TODO: do we need a default value mechanism? How to make sure it does not pop upwards? Current functions do not allow overloads.
                node = New<LearnableParameter<ElemType>>(deviceId, nodeName, (size_t)config[L"rows"], (size_t)config[L"cols"]);
                node->NeedGradient() = config[L"needGradient"];
                static int randomSeed = 1;
                wstring initString = config[L"init"];
                if (initString == L"fixedValue")
                    node->FunctionValues().SetValue((ElemType)config[L"value"]);
                else if (initString == L"uniform" || initString == L"gaussian")
                    ComputationNetwork<ElemType>::InitLearnableParameters(node, (initString == L"uniform"), randomSeed++, config[L"initValueScale"], m_randomSeedOffset);
                else if (initString == L"fromFile")
                {
                    wstring initFromFilePath = config[L"initFromFilePath"];
                    if (initFromFilePath.empty())
                        RuntimeError("initFromFilePath must be set when using \"fromFile\" initialization method");
                    ComputationNetwork<ElemType>::InitLearnableParametersFromFile(node, initFromFilePath, node->GetDeviceId());
                }
                else
                    RuntimeError("init must be one of the values of [uniform|gaussian|fixedValue|fromFile]");
            }
            else if (operationName == L"Input")
            {
                node = New<InputValue<ElemType>>(deviceId, nodeName, (size_t)config[L"rows"], (size_t)config[L"cols"]);
            }
            else        // nodes with inputs
            {
                let inputs = GetInputs(config);
                // second group: nodes with special initializers
                // third group: 
                node = ComputationNetwork<ElemType>::NewStandardNode(operationName, deviceId, nodeName);
                node->AttachInputs(inputs); // TODO: where to check the number of inputs? Should be a template parameter to ComputationNode!
            }
            // add a tag
            let nodeWithTag = dynamic_pointer_cast<WithTag>(node);
            if (nodeWithTag)
                nodeWithTag->SetTag(config[L"tag"]);
            // and done
            return node;
        }

        // -------------------------------------------------------------------
        // ... more specialized node types that have extra constructor parameters
        // -------------------------------------------------------------------

        // fragment from original NDL--optional params are evaluated afterwards, such as initvalue
        // node->EvaluateMacro(nodeEval, baseName, pass);
        // nodeEval.ProcessOptionalParameters(node);
    };

    // creates the lambda for creating an object that can exist as 'float' or 'double'
    // Pass both types as the two template args.
    template<class Cfloat, class Cdouble>
    static ConfigurableRuntimeType MakeRuntimeTypeConstructorDualPrecision()
    {
        ConfigurableRuntimeType rtInfo;
        rtInfo.construct = [](const IConfigRecordPtr config)        // lambda to construct--this lambda can construct both the <float> and the <double> variant based on config parameter 'precision'
        {
            wstring precision = (*config)[L"precision"];            // dispatch on ElemType
            if (precision == L"float")
                return DualPrecisionHelpers<float>::MakeRuntimeObject<Cfloat>(config);
            else if (precision == L"double")
                return DualPrecisionHelpers<double>::MakeRuntimeObject<Cdouble>(config);
            else
                RuntimeError("invalid value for 'precision', must be 'float' or 'double'");
        };
        rtInfo.isConfigRecord = is_base_of<IConfigRecord, Cfloat>::value;
        static_assert(is_base_of<IConfigRecord, Cfloat>::value == is_base_of<IConfigRecord, Cdouble>::value, "");   // we assume that both float and double have the same behavior
        return rtInfo;
    }

    //#define DefineRuntimeType(T) { L#T, MakeRuntimeTypeConstructors<T>() } }
#define DefineRuntimeTypeDualPrecision(T) { L#T, MakeRuntimeTypeConstructorDualPrecision<T<float>,T<double>>() }

    // get information about configurable runtime types
    // This returns a ConfigurableRuntimeType structure which primarily contains a lambda to construct a runtime object from a ConfigRecord ('new' expression).
    const ConfigurableRuntimeType * FindExternalRuntimeTypeInfo(const wstring & typeId)
    {
        // lookup table for "new" expression
        // This table lists all C++ types that can be instantiated from "new" expressions, and gives a constructor lambda and type flags.
        static map<wstring, ConfigurableRuntimeType> configurableRuntimeTypes =
        {
            // ComputationNodes
            DefineRuntimeTypeDualPrecision(ComputationNode),
            DefineRuntimeTypeDualPrecision(ComputationNetwork),
#if 0
            DefineRuntimeType(RecurrentComputationNode),
            // In this experimental state, we only have Node and Network.
            // Once BrainScript becomes the driver of everything, we will add other objects like Readers, Optimizers, and Actions here.
#endif
        };

        // first check our own
        let newIter = configurableRuntimeTypes.find(typeId);
        if (newIter != configurableRuntimeTypes.end())
            return &newIter->second;
        return nullptr; // not found
    }

}}}

namespace Microsoft { namespace MSR { namespace CNTK {

    using namespace Microsoft::MSR;

    // helper that returns 'float' or 'double' depending on ElemType
    template<typename ElemType> static const wchar_t * ElemTypeName();
    template<> static const wchar_t * ElemTypeName<float>()  { return L"float"; }
    template<> static const wchar_t * ElemTypeName<double>() { return L"double"; }

    // build a ComputationNetwork from BrainScript source code
    template<typename ElemType>
    /*virtual*/ /*IComputationNetBuilder::*/ComputationNetwork<ElemType>* ExperimentalNetworkBuilder<ElemType>::BuildNetworkFromDescription(ComputationNetwork<ElemType>*)
    {
        if (!m_net || m_net->GetTotalNumberOfNodes() < 1) //not built yet
        {
            // We interface with outer old CNTK config by taking the inner part, which we get as a string, as BrainScript.
            // We prepend a few standard definitions, and also definition of deviceId and precision, which all objects will pull out again when they are being constructed.
            // BUGBUG: We are not getting TextLocations right in this way! Do we need to inject location markers into the source?
            let expr = BS::ParseConfigString(BS::standardFunctions + BS::computationNodes + BS::commonMacros
                + wstrprintf(L"deviceId = %d ; precision = '%s' ; network = new ComputationNetwork ", (int)m_deviceId, ElemTypeName<ElemType>())  // TODO: check if typeid needs postprocessing
                + m_sourceCode);    // source code has the form [ ... ]
            // evaluate the parse tree--specifically the top-level field 'network'--which will create the network
            let object = EvaluateField(expr, L"network");                               // this comes back as a BS::Object
            let network = dynamic_pointer_cast<ComputationNetwork<ElemType>>(object);   // cast it
            // This should not really fail since we constructed the source code above such that this is the right type.
            // However, it is possible (though currently not meaningful) to locally declare a different 'precision' value.
            // In that case, the network might come back with a different element type. We need a runtime check for that.
            if (!network)
                RuntimeError("BuildNetworkFromDescription: network has the wrong element type (float vs. double)");
            // success
            m_net = network;
        }
        m_net->ResetEvalTimeStamp();
        return m_net.get();
    }

    template class ExperimentalNetworkBuilder<float>;
    template class ExperimentalNetworkBuilder<double>;

}}}