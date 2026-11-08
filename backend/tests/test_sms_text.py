from app.services.sms_text import measure, prepare_sms, to_gsm7


def test_plain_ascii_is_single_gsm7():
    assert measure("Hello") == ("GSM7", 1)


def test_160_vs_161_chars():
    assert measure("a" * 160) == ("GSM7", 1)
    assert measure("a" * 161) == ("GSM7", 2)
    assert measure("a" * 306) == ("GSM7", 2)
    assert measure("a" * 307) == ("GSM7", 3)


def test_extension_chars_count_double():
    assert measure("€" * 80) == ("GSM7", 1)
    assert measure("€" * 81) == ("GSM7", 2)


def test_french_accents_in_gsm7_are_kept():
    # é è ù à are part of GSM-7: no transliteration needed
    assert to_gsm7("Récolte à l'école, où ?") == "Récolte à l'école, où ?"


def test_non_gsm7_french_is_transliterated():
    assert to_gsm7("fenêtre, garçon, maïs, cœur, l’été") == "fenetre, garcon, mais, coeur, l'été"


def test_ucs2_when_no_equivalent():
    enc, seg = measure("Привет")
    assert enc == "UCS2" and seg == 1


def test_markdown_and_think_removed():
    out = prepare_sms("<think>hmm</think>**Conseil** :\n* Séchez\n* Stockez", max_segments=3)
    assert out.text == "Conseil :\n- Séchez\n- Stockez"


def test_truncates_to_max_segments_on_sentence():
    text = "Première phrase courte. " * 30
    out = prepare_sms(text, max_segments=2)
    assert out.truncated and out.segments <= 2 and out.encoding == "GSM7"
    assert out.text.endswith(".")


def test_truncates_without_sentence_adds_ellipsis():
    out = prepare_sms("mot " * 200, max_segments=1)
    assert out.segments == 1 and out.text.endswith("...")
