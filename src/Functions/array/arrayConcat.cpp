#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <Functions/GatherUtils/GatherUtils.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/getLeastSupertype.h>
#include <Interpreters/castColumn.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnConst.h>
#include <Common/typeid_cast.h>
#include <common/range.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}


/// arrayConcat(arr1, ...) - concatenate arrays.
class FunctionArrayConcat : public IFunction
{
public:
    static constexpr auto name = "arrayConcat";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionArrayConcat>(); }

    String getName() const override { return name; }

    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.empty())
            throw Exception{"Function " + getName() + " requires at least one argument.", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        for (auto i : collections::range(0, arguments.size()))
        {
            const auto * array_type = typeid_cast<const DataTypeArray *>(arguments[i].get());
            if (!array_type)
                throw Exception("Argument " + std::to_string(i) + " for function " + getName() + " must be an array but it has type "
                                + arguments[i]->getName() + ".", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }

        return getLeastSupertype(arguments);
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        if (result_type->onlyNull())
            return result_type->createColumnConstWithDefaultValue(input_rows_count);

        size_t rows = input_rows_count;
        size_t num_args = arguments.size();

        Columns preprocessed_columns(num_args);

        for (size_t i = 0; i < num_args; ++i)
        {
            const ColumnWithTypeAndName & arg = arguments[i];
            ColumnPtr preprocessed_column = arg.column;

            if (!arg.type->equals(*result_type))
                preprocessed_column = castColumn(arg, result_type);

            preprocessed_columns[i] = std::move(preprocessed_column);
        }

        std::vector<std::unique_ptr<GatherUtils::IArraySource>> sources;

        for (auto & argument_column : preprocessed_columns)
        {
            bool is_const = false;

            if (const auto * argument_column_const = typeid_cast<const ColumnConst *>(argument_column.get()))
            {
                is_const = true;
                argument_column = argument_column_const->getDataColumnPtr();
            }

            if (const auto * argument_column_array = typeid_cast<const ColumnArray *>(argument_column.get()))
                sources.emplace_back(GatherUtils::createArraySource(*argument_column_array, is_const, rows));
            else
                throw Exception{"Arguments for function " + getName() + " must be arrays.", ErrorCodes::LOGICAL_ERROR};
        }

        auto sink = GatherUtils::concat(sources);

        return sink;
    }

    bool useDefaultImplementationForConstants() const override { return true; }
};


REGISTER_FUNCTION(ArrayConcat)
{
    factory.registerFunction<FunctionArrayConcat>();
}

}
