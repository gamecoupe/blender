/* Apache License, Version 2.0 */

#include "testing/testing.h"

#include "FN_cpp_type.hh"
#include "FN_field.hh"
#include "FN_multi_function_builder.hh"

namespace blender::fn::tests {

TEST(field, ConstantFunction)
{
  Field constant_field = Field(CPPType::get<int>(),
                               std::make_shared<FieldFunction>(FieldFunction(
                                   std::make_unique<CustomMF_Constant<int>>(10), {})),
                               0);

  Array<int> result(4);
  GMutableSpan result_generic(result.as_mutable_span());
  evaluate_fields({&constant_field, 1}, IndexMask(IndexRange(4)), {&result_generic, 1});

  EXPECT_EQ(result[0], 10);
  EXPECT_EQ(result[1], 10);
  EXPECT_EQ(result[2], 10);
  EXPECT_EQ(result[3], 10);
}

// TEST(field, VArrayInput)
// {

//   FieldFunction function = FieldFunction(std::make_unique<IndexFunction>(), {});
//   Field index_field = Field(CPPType::get<int>(), function, 0);

//   Array<int> result_1(4);
//   GMutableSpan result_generic_1(result_1.as_mutable_span());
//   evaluate_fields({&index_field, 1}, IndexMask(IndexRange(4)), {&result_generic_1, 1});
//   EXPECT_EQ(result_1[0], 0);
//   EXPECT_EQ(result_1[1], 1);
//   EXPECT_EQ(result_1[2], 2);
//   EXPECT_EQ(result_1[3], 3);

//   Array<int> result_2(4);
//   GMutableSpan result_generic_2(result_2.as_mutable_span());
//   evaluate_fields({&index_field, 1}, {20, 30, 40, 50}, {&result_generic_2, 1});
//   EXPECT_EQ(result_2[0], 20);
//   EXPECT_EQ(result_2[1], 30);
//   EXPECT_EQ(result_2[2], 40);
//   EXPECT_EQ(result_2[3], 50);
// }

}  // namespace blender::fn::tests
