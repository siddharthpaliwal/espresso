#include <iostream>

#define BOOST_TEST_MODULE AutoParameter test
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "script_interface/auto_parameters/AutoParameter.hpp"

BOOST_AUTO_TEST_CASE(infer_length) {
  using ScriptInterface::infer_length;
  static_assert(infer_length<Vector<11, int>>() == 11, "");
  static_assert(infer_length<int>() == 0, "");
}

BOOST_AUTO_TEST_CASE(direct_binding) {
  using namespace ScriptInterface;
  int i{19};

  auto p = AutoParameter("i", i);

  BOOST_CHECK(p.type == VariantType::INT);

  BOOST_CHECK(boost::get<int>(p.get()) == 19);
  p.set(42);
  BOOST_CHECK(boost::get<int>(p.get()) == 42);
  BOOST_CHECK(i == 42);
}

BOOST_AUTO_TEST_CASE(read_only) {
  using namespace ScriptInterface;
  const int i = 12;

  auto p = AutoParameter("i", i);

  BOOST_CHECK(p.type == VariantType::INT);

  /* Getting should work as usual */
  BOOST_CHECK(boost::get<int>(p.get()) == i);
  /* Setting should throw */
  BOOST_CHECK_EXCEPTION(p.set(2), std::runtime_error,
                        [](std::runtime_error const &e) {
                          return std::string("i is read-only.") == e.what();
                        });
}

BOOST_AUTO_TEST_CASE(user_provided_lambda) {
  using namespace ScriptInterface;
  int i{12};

  auto setter = [&i](Variant const &j) { i = boost::get<int>(j); };
  auto getter = [&i]() { return i; };

  auto p = AutoParameter("i", setter, getter);

  BOOST_CHECK(p.type == VariantType::INT);

  BOOST_CHECK(boost::get<int>(p.get()) == 12);
  p.set(42);
  BOOST_CHECK(boost::get<int>(p.get()) == 42);
  BOOST_CHECK(i == 42);
}
