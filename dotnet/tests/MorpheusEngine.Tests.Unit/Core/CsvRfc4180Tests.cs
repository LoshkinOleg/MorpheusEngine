using FluentAssertions;

namespace MorpheusEngine.Tests.Unit.Core;

[Trait("Category", "Unit")]
public sealed class CsvRfc4180Tests
{
    [Fact]
    // Verifies that splitting an empty CSV string returns no records.
    public void CsvRfc4180_SplitRecords_EmptyString_ReturnsEmptyList()
    {
        var records = CsvRfc4180.SplitRecords(string.Empty);

        records.Should().BeEmpty();
    }

    [Fact]
    // Verifies that a single CSV row without a newline stays as one record.
    public void CsvRfc4180_SplitRecords_SingleRecordWithoutNewline_ReturnsOneRecord()
    {
        var records = CsvRfc4180.SplitRecords("alpha,beta");

        records.Should().Equal("alpha,beta");
    }

    [Fact]
    // Verifies that CRLF line endings split CSV input into separate records.
    public void CsvRfc4180_SplitRecords_CrlfLineEndings_SplitsRecords()
    {
        var records = CsvRfc4180.SplitRecords("alpha,beta\r\ngamma,delta\r\nepsilon,zeta");

        records.Should().Equal(
            "alpha,beta",
            "gamma,delta",
            "epsilon,zeta");
    }

    [Fact]
    // Verifies that carriage-return-only line endings split CSV input into separate records.
    public void CsvRfc4180_SplitRecords_CarriageReturnOnly_SplitsRecords()
    {
        var records = CsvRfc4180.SplitRecords("alpha,beta\rgamma,delta\repsilon,zeta");

        records.Should().Equal(
            "alpha,beta",
            "gamma,delta",
            "epsilon,zeta");
    }

    [Fact]
    // Verifies that a newline inside a quoted field does not split the record.
    public void CsvRfc4180_SplitRecords_NewlineInsideQuotedField_DoesNotSplitRecord()
    {
        var records = CsvRfc4180.SplitRecords("\"alpha\nbeta\",gamma\ndelta,epsilon");

        records.Should().Equal(
            "\"alpha\nbeta\",gamma",
            "delta,epsilon");
    }

    [Fact]
    // Verifies that doubled quotes inside a quoted field do not break record splitting.
    public void CsvRfc4180_SplitRecords_DoubledQuotesInsideQuotedField_DoesNotBreakSplitting()
    {
        var records = CsvRfc4180.SplitRecords("\"alpha\"\"beta\",gamma\ndelta,epsilon");

        records.Should().Equal(
            "\"alpha\"\"beta\",gamma",
            "delta,epsilon");
    }

    [Fact]
    // Verifies that an unclosed quoted field throws an invalid-operation error.
    public void CsvRfc4180_SplitRecords_UnclosedQuote_ThrowsInvalidOperationException()
    {
        var act = () => CsvRfc4180.SplitRecords("\"alpha,beta");

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*unclosed double-quoted field*");
    }

    [Fact]
    // Verifies that parsing a simple CSV row returns each comma-separated field.
    public void CsvRfc4180_ParseRecordFields_SimpleCommaSeparatedValues_ReturnsFields()
    {
        var fields = CsvRfc4180.ParseRecordFields("alpha,beta,gamma");

        fields.Should().Equal("alpha", "beta", "gamma");
    }

    [Fact]
    // Verifies that quoted-field whitespace is preserved while unquoted-field whitespace is trimmed.
    public void CsvRfc4180_ParseRecordFields_QuotedFieldPreservesWhitespaceWhileUnquotedFieldIsTrimmed()
    {
        var fields = CsvRfc4180.ParseRecordFields("\"  alpha  \",  beta  ");

        fields.Should().Equal("  alpha  ", "beta");
    }

    [Fact]
    // Verifies that an empty field between commas is preserved during parsing.
    public void CsvRfc4180_ParseRecordFields_EmptyFieldBetweenCommas_IsPreserved()
    {
        var fields = CsvRfc4180.ParseRecordFields("alpha,,gamma");

        fields.Should().Equal("alpha", string.Empty, "gamma");
    }

    [Fact]
    // Verifies that a comma inside a quoted field stays within the same parsed field.
    public void CsvRfc4180_ParseRecordFields_CommaInsideQuotedField_RemainsSingleField()
    {
        var fields = CsvRfc4180.ParseRecordFields("\"alpha,beta\",gamma");

        fields.Should().Equal("alpha,beta", "gamma");
    }

    [Fact]
    // Verifies that doubled quotes inside a quoted field collapse to a single quote.
    public void CsvRfc4180_ParseRecordFields_DoubledQuotesInsideQuotedField_CollapseToOneQuote()
    {
        var fields = CsvRfc4180.ParseRecordFields("\"alpha\"\"beta\",gamma");

        fields.Should().Equal("alpha\"beta", "gamma");
    }

    [Fact]
    // Verifies that the checked-in lore CSV round-trips to the expected row and column counts.
    public void CsvRfc4180_Roundtrip_RealLoreCsv_YieldsExpectedRowAndColumnCount()
    {
        // This intentionally reads the checked-in lore CSV because the test plan calls for one real-data roundtrip case.
        var csvPath = GetSandcrawlerLoreCsvPath();
        var csvText = File.ReadAllText(csvPath);

        var records = CsvRfc4180.SplitRecords(csvText);
        var nonEmptyRecords = records.Where(record => !string.IsNullOrWhiteSpace(record)).ToArray();
        var parsedRows = nonEmptyRecords.Select(CsvRfc4180.ParseRecordFields).ToArray();

        records.Should().HaveCount(44);
        records[^1].Should().BeEmpty();
        parsedRows.Should().OnlyContain(row => row.Count == 2);
        parsedRows[0].Should().Equal("subject", "data");
    }

    private static string GetSandcrawlerLoreCsvPath()
    {
        return Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                "..",
                "..",
                "..",
                "..",
                "..",
                "..",
                "game_projects",
                "sandcrawler",
                "lore",
                "default_lore_entries.csv"));
    }
}
