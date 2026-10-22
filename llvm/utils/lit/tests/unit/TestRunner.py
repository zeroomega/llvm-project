# RUN: %{python} %s
#
# END.


import os.path
import platform
import unittest

import lit.discovery
import lit.LitConfig
import lit.Test as Test
from lit.TestRunner import (
    ParserKind,
    IntegratedTestKeywordParser,
    parseIntegratedTestScript,
)


class TestIntegratedTestKeywordParser(unittest.TestCase):
    inputTestCase = None

    @staticmethod
    def load_keyword_parser_lit_tests():
        """
        Create and load the LIT test suite and test objects used by
        TestIntegratedTestKeywordParser
        """
        # Create the global config object.
        lit_config = lit.LitConfig.LitConfig(
            progname="lit",
            path=[],
            diagnostic_level="note",
            useValgrind=False,
            valgrindLeakCheck=False,
            valgrindArgs=[],
            noExecute=False,
            debug=False,
            isWindows=(platform.system() == "Windows"),
            order="smart",
            params={},
        )
        TestIntegratedTestKeywordParser.litConfig = lit_config
        # Perform test discovery.
        test_path = os.path.dirname(os.path.dirname(__file__))
        inputs = [os.path.join(test_path, "Inputs/testrunner-custom-parsers/")]
        assert os.path.isdir(inputs[0])
        tests = lit.discovery.find_tests_for_inputs(lit_config, inputs)
        assert len(tests) == 1 and "there should only be one test"
        TestIntegratedTestKeywordParser.inputTestCase = tests[0]

    @staticmethod
    def make_parsers():
        def custom_parse(line_number, line, output):
            if output is None:
                output = []
            output += [part for part in line.split(" ") if part.strip()]
            return output

        return [
            IntegratedTestKeywordParser("MY_TAG.", ParserKind.TAG),
            IntegratedTestKeywordParser("MY_DNE_TAG.", ParserKind.TAG),
            IntegratedTestKeywordParser("MY_LIST:", ParserKind.LIST),
            IntegratedTestKeywordParser("MY_SPACE_LIST:", ParserKind.SPACE_LIST),
            IntegratedTestKeywordParser("MY_BOOL:", ParserKind.BOOLEAN_EXPR),
            IntegratedTestKeywordParser("MY_INT:", ParserKind.INTEGER),
            IntegratedTestKeywordParser("MY_RUN:", ParserKind.COMMAND),
            IntegratedTestKeywordParser("MY_CUSTOM:", ParserKind.CUSTOM, custom_parse),
            IntegratedTestKeywordParser("MY_DEFINE:", ParserKind.DEFINE),
            IntegratedTestKeywordParser("MY_REDEFINE:", ParserKind.REDEFINE),
        ]

    @staticmethod
    def get_parser(parser_list, keyword):
        for p in parser_list:
            if p.keyword == keyword:
                return p
        assert False and "parser not found"

    @staticmethod
    def parse_test(parser_list, allow_result=False):
        script = parseIntegratedTestScript(
            TestIntegratedTestKeywordParser.inputTestCase,
            additional_parsers=parser_list,
            require_script=False,
        )
        if isinstance(script, lit.Test.Result):
            assert allow_result
        else:
            assert isinstance(script, list)
            assert len(script) == 0
        return script

    def test_tags(self):
        parsers = self.make_parsers()
        self.parse_test(parsers)
        tag_parser = self.get_parser(parsers, "MY_TAG.")
        dne_tag_parser = self.get_parser(parsers, "MY_DNE_TAG.")
        self.assertTrue(tag_parser.getValue())
        self.assertFalse(dne_tag_parser.getValue())

    def test_lists(self):
        parsers = self.make_parsers()
        self.parse_test(parsers)
        list_parser = self.get_parser(parsers, "MY_LIST:")
        self.assertEqual(list_parser.getValue(), ["one", "two", "three", "four"])

    def test_space_lists(self):
        parsers = self.make_parsers()
        self.parse_test(parsers)
        space_list_parser = self.get_parser(parsers, "MY_SPACE_LIST:")
        self.assertEqual(
            space_list_parser.getValue(),
            [
                "orange",
                "tabby",
                "tortie",
                "tuxedo",
                "void",
                "multiple",
                "spaces",
                "cute,",
                "fluffy,",
                "kittens",
            ],
        )

    def test_commands(self):
        parsers = self.make_parsers()
        self.parse_test(parsers)
        cmd_parser = self.get_parser(parsers, "MY_RUN:")
        value = cmd_parser.getValue()
        self.assertEqual(len(value), 2)  # there are only two run lines
        self.assertEqual(value[0].command.strip(), "%dbg(MY_RUN: at line 4)  baz")
        self.assertEqual(value[1].command.strip(), "%dbg(MY_RUN: at line 12)  foo  bar")

    def test_boolean(self):
        parsers = self.make_parsers()
        self.parse_test(parsers)
        bool_parser = self.get_parser(parsers, "MY_BOOL:")
        value = bool_parser.getValue()
        self.assertEqual(len(value), 2)  # there are only two run lines
        self.assertEqual(value[0].strip(), "a && (b)")
        self.assertEqual(value[1].strip(), "d")

    def test_integer(self):
        parsers = self.make_parsers()
        self.parse_test(parsers)
        int_parser = self.get_parser(parsers, "MY_INT:")
        value = int_parser.getValue()
        self.assertEqual(len(value), 2)  # there are only two MY_INT: lines
        self.assertEqual(type(value[0]), int)
        self.assertEqual(value[0], 4)
        self.assertEqual(type(value[1]), int)
        self.assertEqual(value[1], 6)

    def test_bad_parser_type(self):
        parsers = self.make_parsers() + ["BAD_PARSER_TYPE"]
        script = self.parse_test(parsers, allow_result=True)
        self.assertTrue(isinstance(script, lit.Test.Result))
        self.assertEqual(script.code, lit.Test.UNRESOLVED)
        self.assertEqual(
            "Additional parser must be an instance of " "IntegratedTestKeywordParser",
            script.output,
        )

    def test_duplicate_keyword(self):
        parsers = self.make_parsers() + [
            IntegratedTestKeywordParser("KEY:", ParserKind.BOOLEAN_EXPR),
            IntegratedTestKeywordParser("KEY:", ParserKind.BOOLEAN_EXPR),
        ]
        script = self.parse_test(parsers, allow_result=True)
        self.assertTrue(isinstance(script, lit.Test.Result))
        self.assertEqual(script.code, lit.Test.UNRESOLVED)
        self.assertEqual("Parser for keyword 'KEY:' already exists", script.output)

    def test_boolean_unterminated(self):
        parsers = self.make_parsers() + [
            IntegratedTestKeywordParser(
                "MY_BOOL_UNTERMINATED:", ParserKind.BOOLEAN_EXPR
            )
        ]
        script = self.parse_test(parsers, allow_result=True)
        self.assertTrue(isinstance(script, lit.Test.Result))
        self.assertEqual(script.code, lit.Test.UNRESOLVED)
        self.assertEqual(
            "Test has unterminated 'MY_BOOL_UNTERMINATED:' lines " "(with '\\')",
            script.output,
        )

    def test_custom(self):
        parsers = self.make_parsers()
        self.parse_test(parsers)
        custom_parser = self.get_parser(parsers, "MY_CUSTOM:")
        value = custom_parser.getValue()
        self.assertEqual(value, ["a", "b", "c"])

    def test_defines(self):
        parsers = self.make_parsers()
        self.parse_test(parsers)
        cmd_parser = self.get_parser(parsers, "MY_DEFINE:")
        value = cmd_parser.getValue()
        self.assertEqual(len(value), 1)  # there's only one MY_DEFINE directive
        self.assertEqual(value[0].new_subst, True)
        self.assertEqual(value[0].name, "%{name}")
        self.assertEqual(value[0].value, "value one")

    def test_redefines(self):
        parsers = self.make_parsers()
        self.parse_test(parsers)
        cmd_parser = self.get_parser(parsers, "MY_REDEFINE:")
        value = cmd_parser.getValue()
        self.assertEqual(len(value), 1)  # there's only one MY_REDEFINE directive
        self.assertEqual(value[0].new_subst, False)
        self.assertEqual(value[0].name, "%{name}")
        self.assertEqual(value[0].value, "value two")

    def test_bad_keywords(self):
        def custom_parse(line_number, line, output):
            return output

        try:
            IntegratedTestKeywordParser("TAG_NO_SUFFIX", ParserKind.TAG),
            self.fail("TAG_NO_SUFFIX failed to raise an exception")
        except ValueError as e:
            pass
        except BaseException as e:
            self.fail("TAG_NO_SUFFIX raised the wrong exception: %r" % e)

        try:
            IntegratedTestKeywordParser("TAG_WITH_COLON:", ParserKind.TAG),
            self.fail("TAG_WITH_COLON: failed to raise an exception")
        except ValueError as e:
            pass
        except BaseException as e:
            self.fail("TAG_WITH_COLON: raised the wrong exception: %r" % e)

        try:
            IntegratedTestKeywordParser("LIST_WITH_DOT.", ParserKind.LIST),
            self.fail("LIST_WITH_DOT. failed to raise an exception")
        except ValueError as e:
            pass
        except BaseException as e:
            self.fail("LIST_WITH_DOT. raised the wrong exception: %r" % e)

        try:
            IntegratedTestKeywordParser("SPACE_LIST_WITH_DOT.", ParserKind.SPACE_LIST),
            self.fail("SPACE_LIST_WITH_DOT. failed to raise an exception")
        except ValueError as e:
            pass
        except BaseException as e:
            self.fail("SPACE_LIST_WITH_DOT. raised the wrong exception: %r" % e)

        try:
            IntegratedTestKeywordParser(
                "CUSTOM_NO_SUFFIX", ParserKind.CUSTOM, custom_parse
            ),
            self.fail("CUSTOM_NO_SUFFIX failed to raise an exception")
        except ValueError as e:
            pass
        except BaseException as e:
            self.fail("CUSTOM_NO_SUFFIX raised the wrong exception: %r" % e)

        # Both '.' and ':' are allowed for CUSTOM keywords.
        try:
            IntegratedTestKeywordParser(
                "CUSTOM_WITH_DOT.", ParserKind.CUSTOM, custom_parse
            ),
        except BaseException as e:
            self.fail("CUSTOM_WITH_DOT. raised an exception: %r" % e)
        try:
            IntegratedTestKeywordParser(
                "CUSTOM_WITH_COLON:", ParserKind.CUSTOM, custom_parse
            ),
        except BaseException as e:
            self.fail("CUSTOM_WITH_COLON: raised an exception: %r" % e)

        try:
            IntegratedTestKeywordParser("CUSTOM_NO_PARSER:", ParserKind.CUSTOM),
            self.fail("CUSTOM_NO_PARSER: failed to raise an exception")
        except ValueError as e:
            pass
        except BaseException as e:
            self.fail("CUSTOM_NO_PARSER: raised the wrong exception: %r" % e)


class TestApplySubtitutions(unittest.TestCase):
    def test_simple(self):
        script = ["echo %bar"]
        substitutions = [("%bar", "hello")]
        result = lit.TestRunner.applySubstitutions(script, substitutions)
        self.assertEqual(result, ["echo hello"])

    def test_multiple_substitutions(self):
        script = ["echo %bar %baz"]
        substitutions = [
            ("%bar", "hello"),
            ("%baz", "world"),
            ("%useless", "shouldnt expand"),
        ]
        result = lit.TestRunner.applySubstitutions(script, substitutions)
        self.assertEqual(result, ["echo hello world"])

    def test_multiple_script_lines(self):
        script = ["%cxx %compile_flags -c -o %t.o", "%cxx %link_flags %t.o -o %t.exe"]
        substitutions = [
            ("%cxx", "clang++"),
            ("%compile_flags", "-std=c++11 -O3"),
            ("%link_flags", "-lc++"),
        ]
        result = lit.TestRunner.applySubstitutions(script, substitutions)
        self.assertEqual(
            result,
            ["clang++ -std=c++11 -O3 -c -o %t.o", "clang++ -lc++ %t.o -o %t.exe"],
        )

    def test_recursive_substitution_real(self):
        script = ["%build %s"]
        substitutions = [
            ("%cxx", "clang++"),
            ("%compile_flags", "-std=c++11 -O3"),
            ("%link_flags", "-lc++"),
            ("%build", "%cxx %compile_flags %link_flags %s -o %t.exe"),
        ]
        result = lit.TestRunner.applySubstitutions(
            script, substitutions, recursion_limit=3
        )
        self.assertEqual(result, ["clang++ -std=c++11 -O3 -lc++ %s -o %t.exe %s"])

    def test_recursive_substitution_limit(self):
        script = ["%rec5"]
        # Make sure the substitutions are not in an order where the global
        # substitution would appear to be recursive just because they are
        # processed in the right order.
        substitutions = [
            ("%rec1", "STOP"),
            ("%rec2", "%rec1"),
            ("%rec3", "%rec2"),
            ("%rec4", "%rec3"),
            ("%rec5", "%rec4"),
        ]
        for limit in [5, 6, 7]:
            result = lit.TestRunner.applySubstitutions(
                script, substitutions, recursion_limit=limit
            )
            self.assertEqual(result, ["STOP"])

    def test_recursive_substitution_limit_exceeded(self):
        script = ["%rec5"]
        substitutions = [
            ("%rec1", "STOP"),
            ("%rec2", "%rec1"),
            ("%rec3", "%rec2"),
            ("%rec4", "%rec3"),
            ("%rec5", "%rec4"),
        ]
        for limit in [0, 1, 2, 3, 4]:
            try:
                lit.TestRunner.applySubstitutions(
                    script, substitutions, recursion_limit=limit
                )
                self.fail("applySubstitutions should have raised an exception")
            except ValueError:
                pass

    def test_recursive_substitution_invalid_value(self):
        script = ["%rec5"]
        substitutions = [
            ("%rec1", "STOP"),
            ("%rec2", "%rec1"),
            ("%rec3", "%rec2"),
            ("%rec4", "%rec3"),
            ("%rec5", "%rec4"),
        ]
        for limit in [-1, -2, -3, "foo"]:
            try:
                lit.TestRunner.applySubstitutions(
                    script, substitutions, recursion_limit=limit
                )
                self.fail("applySubstitutions should have raised an exception")
            except AssertionError:
                pass

    def test_regex_edge_cases(self):
        script = ["ac", "z", "echo %%foo %bar clang-check clang"]
        substitutions = [
            (r"ab{,2}c", "MATCH_QUANT"),
            (r"([]a-z)WRONG[])*[]z]", "MATCH_CC"),
            (r"%bar", "BAR"),
            (r"(?<!\S)clang(?!\S)", "/bin/clang"),
        ]
        result = lit.TestRunner.applySubstitutions(script, substitutions)
        self.assertEqual(
            result,
            ["MATCH_QUANT", "MATCH_CC", "echo %foo BAR clang-check /bin/clang"],
        )

    def test_prepend_left_matches_shparser(self):
        import shlex
        from lit import ShUtil
        from lit.ShCommands import Command

        dbg = "RUN: at line 1"
        for command in ["cmd1 & cmd2", "cmd1 || cmd2 ; cmd3 && cmd4"]:
            expected = ShUtil.ShParser(
                f"@echo '# {dbg}' && @echo {shlex.quote(command.lstrip())} && {command}",
                False,
                False,
            ).parse()
            parsed = ShUtil.ShParser(command, False, False).parse()
            echo_dbg = ShUtil.Pipeline(
                [Command(["@echo", f"# {dbg}"], [])], False, False
            )
            echo_cmd = ShUtil.Pipeline(
                [Command(["@echo", command.lstrip()], [])], False, False
            )
            actual = lit.TestRunner._prepend_left(
                ShUtil.Seq(echo_dbg, "&&", echo_cmd), parsed
            )
            self.assertEqual(actual, expected)

    def test_smart_order_falls_back_to_file_size(self):
        import tempfile
        import lit.main
        import lit.TestingConfig

        lit_config = TestIntegratedTestKeywordParser.litConfig
        with tempfile.TemporaryDirectory() as td:
            cfg = lit.TestingConfig.TestingConfig.fromdefaults(lit_config)
            cfg.name = "suite"
            suite = Test.TestSuite("suite", td, td, cfg)

            small_path = os.path.join(td, "a_small.txt")
            large_path = os.path.join(td, "z_large.txt")
            timed_path = os.path.join(td, "m_timed.txt")
            with open(small_path, "wb") as f:
                f.write(b"x" * 10)
            with open(large_path, "wb") as f:
                f.write(b"x" * 1000)
            with open(timed_path, "wb") as f:
                f.write(b"x" * 5)

            t_small = Test.Test(suite, ("a_small.txt",), cfg)
            t_large = Test.Test(suite, ("z_large.txt",), cfg)
            t_timed = Test.Test(suite, ("m_timed.txt",), cfg)
            t_timed.previous_elapsed = 0.5
            t_gtest = Test.Test(
                suite,
                ("b_gtest", "0", "1"),
                cfg,
                file_path=large_path,
                gtest_json_file="out.json",
            )
            t_missing = Test.Test(suite, ("c_missing.txt",), cfg)

            tests = [t_small, t_gtest, t_missing, t_large, t_timed]
            lit.main.determine_order(tests, "smart")
            self.assertEqual(
                [t.getFullName() for t in tests],
                [
                    "suite :: m_timed.txt",
                    "suite :: z_large.txt",
                    "suite :: a_small.txt",
                    "suite :: b_gtest/0/1",
                    "suite :: c_missing.txt",
                ],
            )


if __name__ == "__main__":
    TestIntegratedTestKeywordParser.load_keyword_parser_lit_tests()
    unittest.main(verbosity=2)
