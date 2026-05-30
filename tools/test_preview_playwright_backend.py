import winnerz


def main():
    doc = winnerz.open("data/ieee-format.pdf")
    pix = doc[0].get_pixmap(matrix=winnerz.Matrix(1.0, 1.0))
    print("PASS", pix.width, pix.height, pix.n)


if __name__ == "__main__":
    main()
